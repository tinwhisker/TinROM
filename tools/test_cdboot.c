#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/iso9660.h"
#include "core/psexe.h"

static FILE *image;

static TinromResult fileReader(uint32_t lba, void *buffer, size_t count, void *user) {
	(void) user;
	if (fseek(image, (long) lba * ISO_SECTOR_SIZE, SEEK_SET)) return TINROM_ERR_IO;
	if (fread(buffer, ISO_SECTOR_SIZE, count, image) != count) return TINROM_ERR_IO;
	return TINROM_OK;
}

/* The SYSTEM.CNF parser the cdboot module uses. */
extern bool tinromParseBootLine(const char *text, size_t length, char *out, size_t size);

static int failures;
static void check(const char *what, int ok) {
	printf("  %-46s %s\n", what, ok ? "ok" : "FAIL");
	if (!ok) failures++;
}

int main(int argc, char **argv) {
	(void) argc;
	image = fopen(argv[1], "rb");
	if (!image) { perror("open"); return 1; }

	Iso9660 fs;
	printf("mount\n");
	check("mounts", iso9660Mount(&fs, fileReader, NULL) == TINROM_OK);
	printf("  label: '%s'  root lba %u size %u\n", fs.label, fs.rootLba, fs.rootSize);
	check("label read and trimmed", !strcmp(fs.label, "TINROMTEST"));
	check("root spans >1 sector", fs.rootSize > ISO_SECTOR_SIZE);

	printf("find\n");
	uint32_t lba = 0, size = 0;
	check("\\SYSTEM.CNF", iso9660Find(&fs, "\\SYSTEM.CNF", &lba, &size) == TINROM_OK);
	check("/system.cnf (case, separator)",
		iso9660Find(&fs, "/system.cnf", &lba, &size) == TINROM_OK);
	check("cdrom:\\SLUS_007.07;1",
		iso9660Find(&fs, "cdrom:\\SLUS_007.07;1", &lba, &size) == TINROM_OK);
	check("SLUS_007.07 without ;1",
		iso9660Find(&fs, "SLUS_007.07", &lba, &size) == TINROM_OK);
	check("subdir\\deep.dat",
		iso9660Find(&fs, "\\SUBDIR\\DEEP.DAT", &lba, &size) == TINROM_OK);
	check("file past the first root sector",
		iso9660Find(&fs, "FILLER39.DAT", &lba, &size) == TINROM_OK);
	check("missing file reports not present",
		iso9660Find(&fs, "NOPE.DAT", &lba, &size) == TINROM_ERR_NOT_PRESENT);
	check("a directory is not a file",
		iso9660Find(&fs, "SUBDIR", &lba, &size) == TINROM_ERR_BAD_ARG);

	printf("listing root\n");
	Iso9660Dir dir;
	unsigned n = 0, dirs = 0;
	check("opens root", iso9660OpenDir(&fs, "/", &dir) == TINROM_OK);
	for (;;) {
		Iso9660Entry e;
		if (iso9660NextEntry(&dir, &e) != TINROM_OK) break;
		if (e.isDirectory) dirs++;
		n++;
	}
	printf("  %u entries, %u directories\n", n, dirs);
	check("43 entries (42 files + SUBDIR)", n == 43);
	check("dot and dotdot skipped", dirs == 1);

	printf("read + parse\n");
	static uint8_t buf[8192];
	uint32_t got = 0;
	check("reads SYSTEM.CNF",
		iso9660ReadFile(&fs, "\\SYSTEM.CNF", buf, sizeof(buf), &got) == TINROM_OK);
	buf[got] = 0;
	check("undersized buffer refused, not overrun",
		iso9660ReadFile(&fs, "\\SYSTEM.CNF", buf, 64, &got) == TINROM_ERR_NO_SPACE);

	char boot[64];
	check("BOOT line parsed",
		tinromParseBootLine((const char *) buf, got, boot, sizeof(boot)));
	printf("  BOOT -> '%s'\n", boot);
	check("BOOT value is the exe", !strcmp(boot, "cdrom:\\SLUS_007.07;1"));

	check("finds what BOOT named",
		iso9660Find(&fs, boot, &lba, &size) == TINROM_OK);

	check("reads the exe header",
		iso9660ReadFile(&fs, boot, buf, sizeof(buf), &got) == TINROM_OK
			|| got > 0);
	uint8_t header[2048];
	fseek(image, (long) lba * ISO_SECTOR_SIZE, SEEK_SET);
	fread(header, 1, 2048, image);

	TinromExeHeader exe;
	check("exe header parses", tinromExeParse(header, &exe) == TINROM_OK);
	printf("  pc %08x gp %08x load %08x+%u sp %08x\n",
		exe.pc, exe.gp, exe.loadAddress, exe.loadSize, exe.spBase + exe.spOffset);
	check("entry point", exe.pc == 0x80010000);
	check("load size", exe.loadSize == 4096);

	uint8_t bad[2048];
	memcpy(bad, header, 2048);
	memcpy(bad, "PS-Y EXE", 8);
	check("bad magic rejected", tinromExeParse(bad, &exe) == TINROM_ERR_CORRUPT);
	memcpy(bad, header, 2048);
	*(uint32_t *) &bad[0x18] = 0x1f000000;   /* EXP1, not RAM */
	check("load address outside RAM rejected",
		tinromExeParse(bad, &exe) == TINROM_ERR_UNSUPPORTED);
	memcpy(bad, header, 2048);
	*(uint32_t *) &bad[0x1c] = 0x00300000;   /* 3 MB into 2 MB of RAM */
	check("oversized payload rejected",
		tinromExeParse(bad, &exe) == TINROM_ERR_UNSUPPORTED);

	printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
	return failures ? 1 : 0;
}
