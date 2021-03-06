/*
 * CXL host parameter parsing routines
 *
 * Copyright (c) 2022 Huawei
 * Modeled loosely on the NUMA options handling in hw/core/numa.c
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/bitmap.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "sysemu/qtest.h"
#include "hw/boards.h"

#include "qapi/qapi-visit-machine.h"
#include "hw/cxl/cxl.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_bridge.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/pcie_port.h"

void cxl_fixed_memory_window_config(MachineState *ms,
                                    CXLFixedMemoryWindowOptions *object,
                                    Error **errp)
{
    CXLFixedWindow *fw = g_malloc0(sizeof(*fw));
    strList *target;
    int i;

    for (target = object->targets; target; target = target->next) {
        fw->num_targets++;
    }

    fw->enc_int_ways = cxl_interleave_ways_enc(fw->num_targets, errp);
    if (*errp) {
        return;
    }

    fw->targets = g_malloc0_n(fw->num_targets, sizeof(*fw->targets));
    for (i = 0, target = object->targets; target; i++, target = target->next) {
        /* This link cannot be resolved yet, so stash the name for now */
        fw->targets[i] = g_strdup(target->value);
    }

    if (object->size % (256 * MiB)) {
        error_setg(errp,
                   "Size of a CXL fixed memory window must my a multiple of 256MiB");
        return;
    }
    fw->size = object->size;

    if (object->has_interleave_granularity) {
        fw->enc_int_gran =
            cxl_interleave_granularity_enc(object->interleave_granularity,
                                           errp);
        if (*errp) {
            return;
        }
    } else {
        /* Default to 256 byte interleave */
        fw->enc_int_gran = 0;
    }

    ms->cxl_devices_state->fixed_windows =
        g_list_append(ms->cxl_devices_state->fixed_windows, fw);

    return;
}

void cxl_fixed_memory_window_link_targets(Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());

    if (ms->cxl_devices_state && ms->cxl_devices_state->fixed_windows) {
        GList *it;

        for (it = ms->cxl_devices_state->fixed_windows; it; it = it->next) {
            CXLFixedWindow *fw = it->data;
            int i;

            for (i = 0; i < fw->num_targets; i++) {
                Object *o;
                bool ambig;

                o = object_resolve_path_type(fw->targets[i],
                                             TYPE_PXB_CXL_DEVICE,
                                             &ambig);
                if (!o) {
                    error_setg(errp, "Could not resolve CXLFM target %s",
                               fw->targets[i]);
                    return;
                }
                fw->target_hbs[i] = PXB_CXL_DEV(o);
            }
        }
    }
}

/* TODO: support, multiple hdm decoders */
static bool cxl_hdm_find_target(uint32_t *cache_mem, hwaddr addr,
                                uint8_t *target)
{
    uint32_t ctrl;
    uint32_t ig_enc;
    uint32_t iw_enc;
    uint32_t target_reg;
    uint32_t target_idx;

    ctrl = cache_mem[R_CXL_HDM_DECODER0_CTRL];
    if (!FIELD_EX32(ctrl, CXL_HDM_DECODER0_CTRL, COMMITTED)) {
        return false;
    }

    ig_enc = FIELD_EX32(ctrl, CXL_HDM_DECODER0_CTRL, IG);
    iw_enc = FIELD_EX32(ctrl, CXL_HDM_DECODER0_CTRL, IW);
    target_idx = (addr / cxl_decode_ig(ig_enc)) % (1 << iw_enc);

    if (target_idx > 4) {
        target_reg = cache_mem[R_CXL_HDM_DECODER0_TARGET_LIST_LO];
        target_reg >>= target_idx * 8;
    } else {
        target_reg = cache_mem[R_CXL_HDM_DECODER0_TARGET_LIST_LO];
        target_reg >>= (target_idx - 4) * 8;
    }
    *target = target_reg & 0xff;

    return true;
}

static PCIDevice *cxl_cfmws_find_device(CXLFixedWindow *fw, hwaddr addr)
{
    CXLComponentState *hb_cstate;
    PCIHostState *hb;
    int rb_index;
    uint32_t *cache_mem;
    uint8_t target;
    bool target_found;
    PCIDevice *rp, *d;

    /* Address is relative to memory region. Convert to HPA */
    addr += fw->base;

    rb_index = (addr / cxl_decode_ig(fw->enc_int_gran)) % fw->num_targets;
    hb = PCI_HOST_BRIDGE(fw->target_hbs[rb_index]->cxl.cxl_host_bridge);
    if (!hb || !hb->bus || !pci_bus_is_cxl(hb->bus)) {
        return NULL;
    }

    hb_cstate = cxl_get_hb_cstate(hb);
    if (!hb_cstate) {
        return NULL;
    }

    cache_mem = hb_cstate->crb.cache_mem_registers;

    target_found = cxl_hdm_find_target(cache_mem, addr, &target);
    if (!target_found) {
        return NULL;
    }

    rp = pcie_find_port_by_pn(hb->bus, target);
    if (!rp) {
        return NULL;
    }

    d = pci_bridge_get_sec_bus(PCI_BRIDGE(rp))->devices[0];

    if (!d || !object_dynamic_cast(OBJECT(d), TYPE_CXL_TYPE3)) {
        return NULL;
    }

    return d;
}

static MemTxResult cxl_read_cfmws(void *opaque, hwaddr addr, uint64_t *data,
                                  unsigned size, MemTxAttrs attrs)
{
    CXLFixedWindow *fw = opaque;
    PCIDevice *d;

    d = cxl_cfmws_find_device(fw, addr);
    if (d == NULL) {
        *data = 0;
        /* Reads to invalid address return poison */
        return MEMTX_ERROR;
    }

    return cxl_type3_read(d, addr + fw->base, data, size, attrs);
}

static MemTxResult cxl_write_cfmws(void *opaque, hwaddr addr,
                                   uint64_t data, unsigned size,
                                   MemTxAttrs attrs)
{
    CXLFixedWindow *fw = opaque;
    PCIDevice *d;

    d = cxl_cfmws_find_device(fw, addr);
    if (d == NULL) {
        /* Writes to invalid address are silent */
        return MEMTX_OK;
    }

    return cxl_type3_write(d, addr + fw->base, data, size, attrs);
}

const MemoryRegionOps cfmws_ops = {
    .read_with_attrs = cxl_read_cfmws,
    .write_with_attrs = cxl_write_cfmws,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
        .unaligned = true,
    },
};
