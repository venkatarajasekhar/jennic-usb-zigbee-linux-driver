
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/ide.h>

int probe_4drives;

module_param_named(probe, probe_4drives, bool, 0);
MODULE_PARM_DESC(probe, "probe for generic IDE chipset with 4 drives/port");

static int __init ide_4drives_init(void)
{
	ide_hwif_t *hwif, *mate;
	u8 idx[4] = { 0xff, 0xff, 0xff, 0xff };
	hw_regs_t hw;

	if (probe_4drives == 0)
		return -ENODEV;

	memset(&hw, 0, sizeof(hw));

	ide_std_init_ports(&hw, 0x1f0, 0x3f6);
	hw.irq = 14;
	hw.chipset = ide_4drives;

	hwif = ide_find_port();
	if (hwif) {
		ide_init_port_hw(hwif, &hw);
		idx[0] = hwif->index;
	}

	mate = ide_find_port();
	if (mate) {
		ide_init_port_hw(mate, &hw);
		mate->drives[0].select.all ^= 0x20;
		mate->drives[1].select.all ^= 0x20;
		idx[1] = mate->index;

		if (hwif) {
			hwif->mate = mate;
			mate->mate = hwif;
			hwif->serialized = mate->serialized = 1;
		}
	}

	ide_device_add(idx, NULL);

	return 0;
}

module_init(ide_4drives_init);

MODULE_AUTHOR("Bartlomiej Zolnierkiewicz");
MODULE_DESCRIPTION("generic IDE chipset with 4 drives/port support");
MODULE_LICENSE("GPL");