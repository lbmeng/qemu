/*
 * NXP S32G emulation
 *
 * Copyright (C) 2023 Bin Meng <bmeng.cn@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/arm/boot.h"
#include "hw/boards.h"
#include "hw/sysbus.h"
#include "hw/cpu/cluster.h"
#include "hw/intc/arm_gic_common.h"
#include "hw/misc/unimp.h"
#include "sysemu/sysemu.h"
#include "target/arm/cpu.h"

#define NXP_S32G_NUM_APU_CPUS   8
#define NXP_S32G_NUM_MPU_CPUS   4

#define NXP_S32G_MAX_LOW_RAM_SIZE   (2 * GiB)
#define NXP_S32G_MAX_HIGH_RAM_SIZE  (2 * GiB)
#define NXP_S32G_MAX_RAM_SIZE       (NXP_S32G_MAX_LOW_RAM_SIZE + \
                                     NXP_S32G_MAX_HIGH_RAM_SIZE)

typedef struct NxpS32gState {
    /*< private >*/
    DeviceState parent_obj;

    /*< public >*/
    CPUClusterState apu_cluster;
    CPUClusterState mpu_cluster;
    ARMCPU apu_cpu[NXP_S32G_NUM_APU_CPUS];

    MemoryRegion isram;
    MemoryRegion *ddr_ram;
    MemoryRegion ddr_ram_low, ddr_ram_high;

    GICState gic;
    MemoryRegion gic_mr[XLNX_ZYNQMP_GIC_REGIONS][XLNX_ZYNQMP_GIC_ALIASES];

    MemoryRegion mr_unimp[XLNX_ZYNQMP_NUM_UNIMP_AREAS];
} NxpS32gState;

#define TYPE_NXP_S32G "nxp-s32g"
OBJECT_DECLARE_SIMPLE_TYPE(NxpS32gState, NXP_S32G)

typedef struct NxpS32gRdbState {
    MachineState parent;
    NxpS32gState soc;
    struct arm_boot_info binfo;
} NxpS32gRdbState;

OBJECT_DECLARE_SIMPLE_TYPE(NxpS32gRdbState, NXP_S32G_RDB)

enum {
    NXP_S32G_QSPI_BUFFER,
    NXP_S32G_SRAM,
    NXP_S32G_LINFLEX0,
    NXP_S32G_GIC_D,
    NXP_S32G_GIC_R,
    NXP_S32G_DRAM_LO,
    NXP_S32G_DRAM_HI,
};

static const MemMapEntry nxp_s32g_memmap[] = {
    [NXP_S32G_QSPI_BUFFER] = {  0x00000000, 256 * MiB },
    [NXP_S32G_ISRAM] =       {  0x34000000,  20 * MiB },
    [NXP_S32G_LINFLEX0] =    {  0x401C8000,  12 * KiB },
    [NXP_S32G_GIC_D] =       {  0x50800000,   1 * MiB },
    [NXP_S32G_GIC_R] =       {  0x50900000,   1 * MiB },
    [NXP_S32G_DRAM_LO] =     {  0x80000000,   2 * GiB },
    [NXP_S32G_DRAM_HI] =     { 0x880000000,   2 * GiB },
};

static const struct UnimpInfo {
    const char *name;
    hwaddr base;
    hwaddr size;
} nxp_s32g_unimp_areas[] = {
    { .name = "ssram", 0x24000000, 32 * KiB },
};

static void nxp_s32g_create_mpu(MachineState *ms, NxpS32gState *s,
                                const char *boot_cpu, Error **errp)
{
    int i;
    int num_rpus = MIN((int)(ms->smp.cpus - NXP_S32G_NUM_APU_CPUS),
                       NXP_S32G_NUM_MPU_CPUS);

    if (num_rpus <= 0) {
        /* Don't create mpu-cluster object if there's nothing to put in it */
        return;
    }

    object_initialize_child(OBJECT(s), "mpu-cluster", &s->mpu_cluster,
                            TYPE_CPU_CLUSTER);
    qdev_prop_set_uint32(DEVICE(&s->mpu_cluster), "cluster-id", 1);

    for (i = 0; i < num_rpus; i++) {
        const char *name;

        object_initialize_child(OBJECT(&s->mpu_cluster), "rpu-cpu[*]",
                                &s->rpu_cpu[i],
                                ARM_CPU_TYPE_NAME("cortex-r5f"));

        name = object_get_canonical_path_component(OBJECT(&s->rpu_cpu[i]));
        if (strcmp(name, boot_cpu)) {
            /*
             * Secondary CPUs start in powered-down state.
             */
            object_property_set_bool(OBJECT(&s->rpu_cpu[i]),
                                     "start-powered-off", true, &error_abort);
        } else {
            s->boot_cpu_ptr = &s->rpu_cpu[i];
        }

        object_property_set_bool(OBJECT(&s->rpu_cpu[i]), "reset-hivecs", true,
                                 &error_abort);
        if (!qdev_realize(DEVICE(&s->rpu_cpu[i]), NULL, errp)) {
            return;
        }
    }

    qdev_realize(DEVICE(&s->mpu_cluster), NULL, &error_fatal);
}

static void nxp_s32g_create_apu_ctrl(NxpS32gState *s, qemu_irq *gic)
{
    SysBusDevice *sbd;
    int i;

    object_initialize_child(OBJECT(s), "apu-ctrl", &s->apu_ctrl,
                            TYPE_NXP_S32G_APU_CTRL);
    sbd = SYS_BUS_DEVICE(&s->apu_ctrl);

    for (i = 0; i < NXP_S32G_NUM_APU_CPUS; i++) {
        g_autofree gchar *name = g_strdup_printf("cpu%d", i);

        object_property_set_link(OBJECT(&s->apu_ctrl), name,
                                 OBJECT(&s->apu_cpu[i]), &error_abort);
    }

    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, APU_ADDR);
    sysbus_connect_irq(sbd, 0, gic[APU_IRQ]);
}

static void nxp_s32g_create_unimp_mmio(NxpS32gState *s)
{
    unsigned int nr;

    for (nr = 0; nr < ARRAY_SIZE(nxp_s32g_unimp_areas); nr++) {
        const struct UnimpInfo *info = &unimp_areas[nr];
        create_unimplemented_device(info->name, info->base, info->size);
    }
}

static void nxp_s32g_init(Object *obj)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    NxpS32gState *s = NXP_S32G(obj);
    int i;
    int num_apus = MIN(ms->smp.cpus, NXP_S32G_NUM_APU_CPUS);

    object_initialize_child(obj, "apu-cluster", &s->apu_cluster,
                            TYPE_CPU_CLUSTER);
    qdev_prop_set_uint32(DEVICE(&s->apu_cluster), "cluster-id", 0);

    for (i = 0; i < num_apus; i++) {
        object_initialize_child(OBJECT(&s->apu_cluster), "apu-cpu[*]",
                                &s->apu_cpu[i],
                                ARM_CPU_TYPE_NAME("cortex-a53"));
    }

    object_initialize_child(obj, "gic", &s->gic, gic_class_name());

    for (i = 0; i < NXP_S32G_NUM_UARTS; i++) {
        object_initialize_child(obj, "uart[*]", &s->uart[i],
                                TYPE_CADENCE_UART);
    }
}

static void nxp_s32g_soc_realize(DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    NxpS32gState *s = NXP_S32G(dev);
    const MemMapEntry *memmap = nxp_s32g_memmap;
    MemoryRegion *system_memory = get_system_memory();
    uint64_t ram_size = memory_region_size(s->ddr_ram);
    int num_apus = MIN(ms->smp.cpus, NXP_S32G_NUM_APU_CPUS);
    const char *boot_cpu = s->boot_cpu ? s->boot_cpu : "apu-cpu[0]";
    uint64_t ddr_low_size, ddr_high_size;
    qemu_irq gic_spi[GIC_NUM_SPI_INTR];
    Error *err = NULL;
    uint8_t i;

    /*
     * Create the DDR Memory Regions from the SoC perspective.
     * User friendly checks should happen at the board level.
     */
    if (ram_size > NXP_S32G_MAX_LOW_RAM_SIZE) {
        /*
         * The RAM size is above the maximum available for the low DDR.
         * Create the high DDR memory region as well.
         */
        assert(ram_size <= NXP_S32G_MAX_RAM_SIZE);
        ddr_low_size = memmap[NXP_S32G_DRAM_LO].size;
        ddr_high_size = ram_size - ddr_low_size;

        memory_region_init_alias(&s->ddr_ram_high, OBJECT(dev),
                                 "ddr-ram-high", s->ddr_ram,
                                 ddr_low_size, ddr_high_size);
        memory_region_add_subregion(system_memory,
                                    memmap[NXP_S32G_DRAM_HI].base,
                                    &s->ddr_ram_high);
    } else {
        /* RAM must be non-zero */
        assert(ram_size);
        ddr_low_size = ram_size;
    }

    memory_region_init_alias(&s->ddr_ram_low, OBJECT(dev), "ddr-ram-low",
                             s->ddr_ram, 0, ddr_low_size);
    memory_region_add_subregion(system_memory,
                                memmap[NXP_S32G_DRAM_LO].base,
                                &s->ddr_ram_low);

    /* Create the internal SRAM */
    memory_region_init_ram(&s->isram, NULL, "isram",
                           memmap[NXP_S32G_ISRAM].size, &error_fatal);
    memory_region_add_subregion(system_memory,
                                memmap[NXP_S32G_ISRAM].base,
                                &s->isram);

    /* Realize APUs before realizing the GIC as KVM requires this */
    qdev_realize(DEVICE(&s->apu_cluster), NULL, &error_fatal);

    /* GICv3 */
    qdev_prop_set_uint32(DEVICE(&s->gic), "num-cpu", num_apus);
    qdev_prop_set_uint32(DEVICE(&s->gic), "num-irq", GIC_NUM_SPI_INTR + 32);
    qdev_prop_set_uint32(DEVICE(&s->gic), "revision", 3);
    qdev_prop_set_bit(DEVICE(&s->gic), "has-security-extensions", true);

    for (i = 0; i < num_apus; i++) {
        const char *name;

        name = object_get_canonical_path_component(OBJECT(&s->apu_cpu[i]));
        if (strcmp(name, boot_cpu)) {
            /*
             * Secondary CPUs start in powered-down state.
             */
            object_property_set_bool(OBJECT(&s->apu_cpu[i]),
                                     "start-powered-off", true, &error_abort);
        } else {
            s->boot_cpu_ptr = &s->apu_cpu[i];
        }

        object_property_set_bool(OBJECT(&s->apu_cpu[i]), "has_el3", true, NULL);
        object_property_set_bool(OBJECT(&s->apu_cpu[i]), "has_el2", true, NULL);
        object_property_set_int(OBJECT(&s->apu_cpu[i]), "reset-cbar",
                                GIC_BASE_ADDR, &error_abort);
        object_property_set_int(OBJECT(&s->apu_cpu[i]), "core-count",
                                num_apus, &error_abort);
        if (!qdev_realize(DEVICE(&s->apu_cpu[i]), NULL, errp)) {
            return;
        }
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gic), errp)) {
        return;
    }

    assert(ARRAY_SIZE(nxp_s32g_gic_regions) == NXP_S32G_GIC_REGIONS);
    for (i = 0; i < NXP_S32G_GIC_REGIONS; i++) {
        SysBusDevice *gic = SYS_BUS_DEVICE(&s->gic);
        const XlnxZynqMPGICRegion *r = &nxp_s32g_gic_regions[i];
        MemoryRegion *mr;
        uint32_t addr = r->address;
        int j;

        if (r->virt && !s->virt) {
            continue;
        }

        mr = sysbus_mmio_get_region(gic, r->region_index);
        for (j = 0; j < NXP_S32G_GIC_ALIASES; j++) {
            MemoryRegion *alias = &s->gic_mr[i][j];

            memory_region_init_alias(alias, OBJECT(s), "zynqmp-gic-alias", mr,
                                     r->offset, NXP_S32G_GIC_REGION_SIZE);
            memory_region_add_subregion(system_memory, addr, alias);

            addr += NXP_S32G_GIC_REGION_SIZE;
        }
    }

    for (i = 0; i < num_apus; i++) {
        qemu_irq irq;

        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), i,
                           qdev_get_gpio_in(DEVICE(&s->apu_cpu[i]),
                                            ARM_CPU_IRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), i + num_apus,
                           qdev_get_gpio_in(DEVICE(&s->apu_cpu[i]),
                                            ARM_CPU_FIQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), i + num_apus * 2,
                           qdev_get_gpio_in(DEVICE(&s->apu_cpu[i]),
                                            ARM_CPU_VIRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), i + num_apus * 3,
                           qdev_get_gpio_in(DEVICE(&s->apu_cpu[i]),
                                            ARM_CPU_VFIQ));
        irq = qdev_get_gpio_in(DEVICE(&s->gic),
                               arm_gic_ppi_index(i, ARM_PHYS_TIMER_PPI));
        qdev_connect_gpio_out(DEVICE(&s->apu_cpu[i]), GTIMER_PHYS, irq);
        irq = qdev_get_gpio_in(DEVICE(&s->gic),
                               arm_gic_ppi_index(i, ARM_VIRT_TIMER_PPI));
        qdev_connect_gpio_out(DEVICE(&s->apu_cpu[i]), GTIMER_VIRT, irq);
        irq = qdev_get_gpio_in(DEVICE(&s->gic),
                               arm_gic_ppi_index(i, ARM_HYP_TIMER_PPI));
        qdev_connect_gpio_out(DEVICE(&s->apu_cpu[i]), GTIMER_HYP, irq);
        irq = qdev_get_gpio_in(DEVICE(&s->gic),
                               arm_gic_ppi_index(i, ARM_SEC_TIMER_PPI));
        qdev_connect_gpio_out(DEVICE(&s->apu_cpu[i]), GTIMER_SEC, irq);

        if (s->virt) {
            irq = qdev_get_gpio_in(DEVICE(&s->gic),
                                   arm_gic_ppi_index(i, GIC_MAINTENANCE_PPI));
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), i + num_apus * 4, irq);
        }
    }

    nxp_s32g_create_rpu(ms, s, boot_cpu, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    if (!s->boot_cpu_ptr) {
        error_setg(errp, "ZynqMP Boot cpu %s not found", boot_cpu);
        return;
    }

    for (i = 0; i < NXP_S32G_NUM_UARTS; i++) {
        qdev_prop_set_chr(DEVICE(&s->uart[i]), "chardev", serial_hd(i));
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->uart[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->uart[i]), 0, uart_addr[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->uart[i]), 0,
                           gic_spi[uart_intr[i]]);
    }
}

static Property nxp_s32g_soc_props[] = {
    DEFINE_PROP_STRING("boot-cpu", NxpS32gState, boot_cpu),
    DEFINE_PROP_LINK("ddr-ram", NxpS32gState, ddr_ram,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_END_OF_LIST()
};

static void nxp_s32g_soc_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, nxp_s32g_soc_props);
    dc->realize = nxp_s32g_soc_realize;
    /* Reason: Uses serial_hds in realize function, thus can't be used twice */
    dc->user_creatable = false;
}

static const TypeInfo nxp_s32g_soc_type_info = {
    .name = TYPE_NXP_S32G,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(NxpS32gState),
    .instance_init = nxp_s32g_soc_instance_init,
    .class_init = nxp_s32g_soc_class_init,
};

static void nxp_s32g_soc_register_types(void)
{
    type_register_static(&nxp_s32g_soc_type_info);
}

type_init(nxp_s32g_soc_register_types)

static void nxp_s32g_rdb_machine_init(MachineState *machine)
{
    NxpS32gRdbState *s = NXP_S32G_RDB(machine);
    const MemMapEntry *memmap = nxp_s32g_memmap;
    MemoryRegion *system_memory = get_system_memory();
    uint64_t ram_size = machine->ram_size;
    int i;

     /* Sanity check on RAM size */
    if (ram_size > NXP_S32G_MAX_RAM_SIZE) {
        error_report("ERROR: RAM size 0x%" PRIx64 " above max supported of "
                     "0x%llx", ram_size, NXP_S32G_MAX_RAM_SIZE);
        exit(EXIT_FAILURE);
    }

    /* Initialize SoC */
    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_NXP_S32G);
    object_property_set_link(OBJECT(&s->soc), "ddr-ram", OBJECT(machine->ram),
                             &error_abort);
    qdev_realize(DEVICE(&s->soc), NULL, &error_fatal);

    /* TODO: add more peripherals */

    s->binfo.ram_size = ram_size;
    s->binfo.loader_start = 0;
    s->binfo.modify_dtb = zcu102_modify_dtb;
    s->binfo.psci_conduit = QEMU_PSCI_CONDUIT_SMC;
    arm_load_kernel(s->soc.boot_cpu_ptr, machine, &s->binfo);
}

static void nxp_s32g_rdb_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "NXP S32G RDB board";
    mc->init = nxp_s32g_rdb_machine_init;
    mc->max_cpus = NXP_S32G_NUM_APU_CPUS + NXP_S32G_NUM_MPU_CPUS;
    mc->min_cpus = 1;
    mc->default_cpus = mc->min_cpus;
    mc->default_ram_id = "ddr-ram";
}

static void nxp_s32g_rdb_machine_instance_init(Object *obj)
{
}

static const TypeInfo nxp_s32g_rdb_machine_typeinfo = {
    .name       = MACHINE_TYPE_NAME("nxp-s32g-rdb"),
    .parent     = TYPE_MACHINE,
    .class_init = nxp_s32g_rdb_machine_class_init,
    .instance_init = nxp_s32g_rdb_machine_instance_init,
    .instance_size = sizeof(NxpS32gRdbState),
};

static void nxp_s32g_rdb_machine_init_register_types(void)
{
    type_register_static(&nxp_s32g_rdb_machine_typeinfo);
}

type_init(nxp_s32g_rdb_machine_init_register_types)
