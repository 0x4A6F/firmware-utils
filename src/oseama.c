// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * oseama
 *
 * Copyright (C) 2016 Rafał Miłecki <zajec5@gmail.com>
 */

#include <byteswap.h>
#include <endian.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "md5.h"

#if !defined(__BYTE_ORDER)
#error "Unknown byte order"
#endif

#if __BYTE_ORDER == __BIG_ENDIAN
#define cpu_to_be32(x)	(x)
#define be32_to_cpu(x)	(x)
#define cpu_to_be16(x)	(x)
#define be16_to_cpu(x)	(x)
#elif __BYTE_ORDER == __LITTLE_ENDIAN
#define cpu_to_be32(x)	bswap_32(x)
#define be32_to_cpu(x)	bswap_32(x)
#define cpu_to_be16(x)	bswap_16(x)
#define be16_to_cpu(x)	bswap_16(x)
#else
#error "Unsupported endianness"
#endif

#define SEAMA_MAGIC			0x5ea3a417

struct seama_seal_header {
	uint32_t magic;
	uint16_t reserved;
	uint16_t metasize;
	uint32_t imagesize;
} __attribute__ ((packed));

struct seama_entity_header {
	uint32_t magic;
	uint16_t reserved;
	uint16_t metasize;
	uint32_t imagesize;
	uint8_t md5[16];
} __attribute__ ((packed));

char *seama_path;
int entity_idx = -1;
char *out_path;

static inline size_t oseama_min(size_t x, size_t y) {
	return x < y ? x : y;
}

/**************************************************
 * Helpers
 **************************************************/

static FILE *oseama_open(const char *pathname, const char *mode) {
	if (strcmp(pathname, "-"))
		return fopen(pathname, mode);

	if (isatty(fileno(stdin))) {
		fprintf(stderr, "Reading from TTY stdin is unsupported\n");
		return NULL;
	}

	return stdin;
}

static int oseama_skip(FILE *fp, size_t length)
{
	if (fseek(fp, length, SEEK_CUR)) {
		uint8_t buf[1024];
		size_t bytes;

		do {
			bytes = fread(buf, 1, oseama_min(sizeof(buf), length), fp);
			if (bytes <= 0)
				return -EIO;
			length -= bytes;
		} while (length);
	}

	return 0;
}

static void oseama_close(FILE *fp) {
	if (fp != stdin)
		fclose(fp);
}

/**************************************************
 * Info
 **************************************************/

static void oseama_info_parse_options(int argc, char **argv) {
	int c;

	while ((c = getopt(argc, argv, "e:")) != -1) {
		switch (c) {
		case 'e':
			entity_idx = atoi(optarg);
			break;
		}
	}
}

static int oseama_info_entities(FILE *seama, size_t *pos) {
	struct seama_entity_header hdr;
	size_t bytes, metasize, imagesize;
	uint8_t buf[1024];
	char *end, *tmp;
	int i = 0;
	int err = 0;

	while ((bytes = fread(&hdr, 1, sizeof(hdr), seama)) == sizeof(hdr)) {
		*pos += bytes;

		if (be32_to_cpu(hdr.magic) != SEAMA_MAGIC) {
			fprintf(stderr, "Invalid Seama magic: 0x%08x\n", be32_to_cpu(hdr.magic));
			err =  -EINVAL;
			goto err_out;
		}
		metasize = be16_to_cpu(hdr.metasize);
		imagesize = be32_to_cpu(hdr.imagesize);

		if (entity_idx >= 0 && i != entity_idx) {
			oseama_skip(seama, metasize + imagesize);
			*pos += metasize + imagesize;
			i++;
			continue;
		}

		if (metasize >= sizeof(buf)) {
			fprintf(stderr, "Too small buffer (%zu B) to read all meta info (%zd B)\n", sizeof(buf), metasize);
			err =  -EINVAL;
			goto err_out;
		}

		if (entity_idx < 0)
			printf("\n");
		printf("Entity offset:\t%ld\n", *pos - sizeof(hdr));
		printf("Entity size:\t%zd\n", sizeof(hdr) + metasize + imagesize);
		printf("Meta size:\t%zd\n", metasize);
		printf("Image size:\t%zd\n", imagesize);

		bytes = fread(buf, 1, metasize, seama);
		if (bytes != metasize) {
			fprintf(stderr, "Couldn't read %zd B of meta\n", metasize);
			err =  -EIO;
			goto err_out;
		}
		*pos += bytes;

		end = (char *)&buf[metasize - 1];
		*end = '\0';
		for (tmp = (char *)buf; tmp < end && strlen(tmp); tmp += strlen(tmp) + 1) {
			printf("Meta entry:\t%s\n", tmp);
		}

		oseama_skip(seama, imagesize);
		*pos += imagesize;
		i++;
	}

err_out:
	return err;
}

static int oseama_info(int argc, char **argv) {
	FILE *seama;
	struct seama_seal_header hdr;
	size_t bytes;
	uint16_t metasize;
	uint32_t imagesize;
	uint8_t buf[1024];
	size_t pos = 0;
	int err = 0;

	if (argc < 3) {
		fprintf(stderr, "No Seama file passed\n");
		err = -EINVAL;
		goto out;
	}
	seama_path = argv[2];

	optind = 3;
	oseama_info_parse_options(argc, argv);

	seama = oseama_open(seama_path, "r");
	if (!seama) {
		fprintf(stderr, "Couldn't open %s\n", seama_path);
		err = -EACCES;
		goto out;
	}

	bytes = fread(&hdr, 1, sizeof(hdr), seama);
	if (bytes != sizeof(hdr)) {
		fprintf(stderr, "Couldn't read %s header\n", seama_path);
		err =  -EIO;
		goto err_close;
	}
	pos += bytes;

	metasize = be16_to_cpu(hdr.metasize);
	imagesize = be32_to_cpu(hdr.imagesize);

	if (be32_to_cpu(hdr.magic) != SEAMA_MAGIC) {
		fprintf(stderr, "Invalid Seama magic: 0x%08x\n", be32_to_cpu(hdr.magic));
		err =  -EINVAL;
		goto err_close;
	}

	if (metasize >= sizeof(buf)) {
		fprintf(stderr, "Too small buffer (%zu B) to read all meta info (%d B)\n", sizeof(buf), metasize);
		err =  -EINVAL;
		goto err_close;
	}

	if (imagesize) {
		fprintf(stderr, "Invalid Seama image size: 0x%08x (should be 0)\n", imagesize);
		err =  -EINVAL;
		goto err_close;
	}

	bytes = fread(buf, 1, metasize, seama);
	if (bytes != metasize) {
		fprintf(stderr, "Couldn't read %d B of meta\n", metasize);
		err =  -EIO;
		goto err_close;
	}
	pos += bytes;

	if (entity_idx < 0) {
		char *end, *tmp;

		printf("Meta size:\t%d\n", metasize);
		printf("Image size:\t%d\n", imagesize);

		end = (char *)&buf[metasize - 1];
		*end = '\0';
		for (tmp = (char *)buf; tmp < end && strlen(tmp); tmp += strlen(tmp) + 1) {
			printf("Meta entry:\t%s\n", tmp);
		}
	}

	oseama_info_entities(seama, &pos);

err_close:
	oseama_close(seama);
out:
	return err;
}

/**************************************************
 * Create
 **************************************************/

static ssize_t oseama_entity_append_file(FILE *seama, const char *in_path) {
	FILE *in;
	size_t bytes;
	ssize_t length = 0;
	uint8_t buf[128];

	in = fopen(in_path, "r");
	if (!in) {
		fprintf(stderr, "Couldn't open %s\n", in_path);
		return -EACCES;
	}

	while ((bytes = fread(buf, 1, sizeof(buf), in)) > 0) {
		if (fwrite(buf, 1, bytes, seama) != bytes) {
			fprintf(stderr, "Couldn't write %zu B to %s\n", bytes, seama_path);
			length = -EIO;
			break;
		}
		length += bytes;
	}

	fclose(in);

	return length;
}

static ssize_t oseama_entity_append_zeros(FILE *seama, size_t length) {
	uint8_t *buf;

	buf = malloc(length);
	if (!buf)
		return -ENOMEM;
	memset(buf, 0, length);

	if (fwrite(buf, 1, length, seama) != length) {
		fprintf(stderr, "Couldn't write %zu B to %s\n", length, seama_path);
		return -EIO;
	}

	return length;
}

static ssize_t oseama_entity_align(FILE *seama, size_t curr_offset, size_t alignment) {
	if (curr_offset & (alignment - 1)) {
		size_t length = alignment - (curr_offset % alignment);

		return oseama_entity_append_zeros(seama, length);
	}

	return 0;
}

static int oseama_entity_write_hdr(FILE *seama, size_t metasize, size_t imagesize) {
	struct seama_entity_header hdr = {};
	uint8_t buf[128];
	size_t length = imagesize;
	size_t bytes;
	MD5_CTX ctx;

	fseek(seama, sizeof(hdr) + metasize, SEEK_SET);
	MD5_Init(&ctx);
	while ((bytes = fread(buf, 1, oseama_min(sizeof(buf), length), seama)) > 0) {
		MD5_Update(&ctx, buf, bytes);
		length -= bytes;
	}
	MD5_Final(hdr.md5, &ctx);

	hdr.magic = cpu_to_be32(SEAMA_MAGIC);
	hdr.metasize = cpu_to_be16(metasize);
	hdr.imagesize = cpu_to_be32(imagesize);

	fseek(seama, 0, SEEK_SET);
	bytes = fwrite(&hdr, 1, sizeof(hdr), seama);
	if (bytes != sizeof(hdr)) {
		fprintf(stderr, "Couldn't write Seama entity header to %s\n", seama_path);
		return -EIO;
	}

	return 0;
}

static int oseama_entity(int argc, char **argv) {
	FILE *seama;
	ssize_t sbytes;
	size_t curr_offset = sizeof(struct seama_entity_header);
	size_t metasize = 0, imagesize = 0;
	int c;
	int err = 0;

	if (argc < 3) {
		fprintf(stderr, "No Seama file passed\n");
		err = -EINVAL;
		goto out;
	}
	seama_path = argv[2];

	seama = fopen(seama_path, "w+");
	if (!seama) {
		fprintf(stderr, "Couldn't open %s\n", seama_path);
		err = -EACCES;
		goto out;
	}
	fseek(seama, curr_offset, SEEK_SET);

	optind = 3;
	while ((c = getopt(argc, argv, "m:f:b:")) != -1) {
		switch (c) {
		case 'm':
			sbytes = fwrite(optarg, 1, strlen(optarg) + 1, seama);
			if (sbytes < 0) {
				fprintf(stderr, "Failed to write meta %s\n", optarg);
			} else {
				curr_offset += sbytes;
				metasize += sbytes;
			}

			sbytes = oseama_entity_align(seama, curr_offset, 4);
			if (sbytes < 0) {
				fprintf(stderr, "Failed to append zeros\n");
			} else {
				curr_offset += sbytes;
				metasize += sbytes;
			}

			break;
		case 'f':
		case 'b':
			break;
		}
	}

	optind = 3;
	while ((c = getopt(argc, argv, "m:f:b:")) != -1) {
		switch (c) {
		case 'm':
			break;
		case 'f':
			sbytes = oseama_entity_append_file(seama, optarg);
			if (sbytes < 0) {
				fprintf(stderr, "Failed to append file %s\n", optarg);
			} else {
				curr_offset += sbytes;
				imagesize += sbytes;
			}
			break;
		case 'b':
			sbytes = strtol(optarg, NULL, 0) - curr_offset;
			if (sbytes < 0) {
				fprintf(stderr, "Current Seama entity length is 0x%zx, can't pad it with zeros to 0x%lx\n", curr_offset, strtol(optarg, NULL, 0));
			} else {
				sbytes = oseama_entity_append_zeros(seama, sbytes);
				if (sbytes < 0) {
					fprintf(stderr, "Failed to append zeros\n");
				} else {
					curr_offset += sbytes;
					imagesize += sbytes;
				}
			}
			break;
		}
		if (err)
			break;
	}

	oseama_entity_write_hdr(seama, metasize, imagesize);

	fclose(seama);
out:
	return err;
}

/**************************************************
 * Extract
 **************************************************/

static void oseama_extract_parse_options(int argc, char **argv) {
	int c;

	while ((c = getopt(argc, argv, "e:o:")) != -1) {
		switch (c) {
		case 'e':
			entity_idx = atoi(optarg);
			break;
		case 'o':
			out_path = optarg;
			break;
		}
	}
}

static int oseama_extract_entity(FILE *seama, FILE *out) {
	struct seama_entity_header hdr;
	size_t bytes, metasize, imagesize, length;
	uint8_t buf[1024];
	int i = 0;
	int err = 0;

	while ((bytes = fread(&hdr, 1, sizeof(hdr), seama)) == sizeof(hdr)) {
		if (be32_to_cpu(hdr.magic) != SEAMA_MAGIC) {
			fprintf(stderr, "Invalid Seama magic: 0x%08x\n", be32_to_cpu(hdr.magic));
			err =  -EINVAL;
			break;
		}
		metasize = be16_to_cpu(hdr.metasize);
		imagesize = be32_to_cpu(hdr.imagesize);

		if (i != entity_idx) {
			oseama_skip(seama, metasize + imagesize);
			i++;
			continue;
		}

		if (fwrite(&hdr, 1, sizeof(hdr), out) != sizeof(hdr)) {
			fprintf(stderr, "Couldn't write %zu B to %s\n", sizeof(hdr), out_path);
			err = -EIO;
			break;
		}

		length = metasize + imagesize;
		while ((bytes = fread(buf, 1, oseama_min(sizeof(buf), length), seama)) > 0) {
			if (fwrite(buf, 1, bytes, out) != bytes) {
				fprintf(stderr, "Couldn't write %zu B to %s\n", bytes, out_path);
				err = -EIO;
				break;
			}
			length -= bytes;
		}

		if (length) {
			fprintf(stderr, "Couldn't extract whole entity %d from %s (%zu B left)\n", entity_idx, seama_path, length);
			err = -EIO;
			break;
		}

		break;
	}

	return err;
}

static int oseama_extract(int argc, char **argv) {
	FILE *seama;
	FILE *out;
	struct seama_seal_header hdr;
	size_t bytes;
	uint16_t metasize;
	int err = 0;

	if (argc < 3) {
		fprintf(stderr, "No Seama file passed\n");
		err = -EINVAL;
		goto out;
	}
	seama_path = argv[2];

	optind = 3;
	oseama_extract_parse_options(argc, argv);
	if (entity_idx < 0) {
		fprintf(stderr, "No entity specified\n");
		err = -EINVAL;
		goto out;
	}

	seama = oseama_open(seama_path, "r");
	if (!seama) {
		fprintf(stderr, "Couldn't open %s\n", seama_path);
		err = -EACCES;
		goto out;
	}

	if (out_path) {
		out = fopen(out_path, "w");
		if (!out) {
			fprintf(stderr, "Couldn't open %s\n", out_path);
			err = -EACCES;
			goto err_close_seama;
		}
	} else {
		out = stdout;
	}

	bytes = fread(&hdr, 1, sizeof(hdr), seama);
	if (bytes != sizeof(hdr)) {
		fprintf(stderr, "Couldn't read %s header\n", seama_path);
		err =  -EIO;
		goto err_close_out;
	}
	metasize = be16_to_cpu(hdr.metasize);

	oseama_skip(seama, metasize);

	oseama_extract_entity(seama, out);

err_close_out:
	if (out != stdout)
		fclose(out);
err_close_seama:
	oseama_close(seama);
out:
	return err;
}

/**************************************************
 * Start
 **************************************************/

static void usage() {
	printf("Usage:\n");
	printf("\n");
	printf("Info about Seama seal (container):\n");
	printf("\toseama info <file> [options]\n");
	printf("\t-e\t\t\t\tprint info about specified entity only\n");
	printf("\n");
	printf("Create Seama entity:\n");
	printf("\toseama entity <file> [options]\n");
	printf("\t-m meta\t\t\t\tmeta into to put in header\n");
	printf("\t-f file\t\t\t\tappend content from file\n");
	printf("\t-b offset\t\t\tappend zeros till reaching absolute offset\n");
	printf("\n");
	printf("Extract from Seama seal (container):\n");
	printf("\toseama extract <file> [options]\n");
	printf("\t-e\t\t\t\tindex of entity to extract\n");
	printf("\t-o file\t\t\t\toutput file\n");
}

int main(int argc, char **argv) {
	if (argc > 1) {
		if (!strcmp(argv[1], "info"))
			return oseama_info(argc, argv);
		else if (!strcmp(argv[1], "entity"))
			return oseama_entity(argc, argv);
		else if (!strcmp(argv[1], "extract"))
			return oseama_extract(argc, argv);
	}

	usage();
	return 0;
}
