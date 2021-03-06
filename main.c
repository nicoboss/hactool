#include <getopt.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "utils.h"
#include "settings.h"
#include "pki.h"
#include "nca.h"
#include "xci.h"

static char *prog_name = "hactool";

/* Print usage. Taken largely from ctrtool. */
static void usage(void) {
    fprintf(stderr, 
        "hactool (c) SciresM.\n"
        "Built: %s %s\n"
        "\n"
        "Usage: %s [options...] <file>\n"
        "Options:\n"
        "-i, --info        Show file info.\n"
        "                      This is the default action.\n"
        "-x, --extract     Extract data from file.\n"
        "                      This is also the default action.\n"
        "  -r, --raw          Keep raw data, don't unpack.\n"
        "  -y, --verify       Verify hashes and signatures.\n"
        "  -d, --dev          Decrypt with development keys instead of retail.\n"
        "  -t, --intype=type  Specify input file type [nca, xci, pfs0, romfs, hfs0]\n"
        "  --titlekey=key     Set title key for Rights ID crypto titles.\n"
        "  --contentkey=key   Set raw key for NCA body decryption.\n"
        "NCA options:\n"
        "  --plaintext=file   Specify file path for saving a decrypted copy of the NCA.\n"
        "  --header=file      Specify Header file path.\n"
        "  --section0=file    Specify Section 0 file path.\n"
        "  --section1=file    Specify Section 1 file path.\n"
        "  --section2=file    Specify Section 2 file path.\n"
        "  --section3=file    Specify Section 3 file path.\n"
        "  --section0dir=dir  Specify Section 0 directory path.\n"
        "  --section1dir=dir  Specify Section 1 directory path.\n"
        "  --section2dir=dir  Specify Section 2 directory path.\n"
        "  --section3dir=dir  Specify Section 3 directory path.\n"
        "  --exefs=file       Specify ExeFS file path. Overrides appropriate section file path.\n"
        "  --exefsdir=dir     Specify ExeFS directory path. Overrides appropriate section directory path.\n"
        "  --romfs=file       Specify RomFS file path. Overrides appropriate section file path.\n"
        "  --romfsdir=dir     Specify RomFS directory path. Overrides appropriate section directory path.\n"
        "  --listromfs        List files in RomFS.\n"
        "  --baseromfs        Set Base RomFS to use with update partitions.\n"
        "  --basenca          Set Base NCA to use with update partitions.\n" 
        "PFS0 options:\n"
        "  --pfs0dir=dir      Specify PFS0 directory path.\n"
        "  --outdir=dir       Specify PFS0 directory path. Overrides previous path, if present.\n"
        "  --exefsdir=dir     Specify PFS0 directory path. Overrides previous paths, if present for ExeFS PFS0.\n"
        "RomFS options:\n"
        "  --romfsdir=dir     Specify RomFS directory path.\n"
        "  --outdir=dir       Specify RomFS directory path. Overrides previous path, if present.\n"
        "  --listromfs        List files in RomFS.\n"
        "HFS0 options:\n"
        "  --hfs0dir=dir      Specify HFS0 directory path.\n"
        "  --outdir=dir       Specify HFS0 directory path. Overrides previous path, if present.\n"
        "  --exefsdir=dir     Specify HFS0 directory path. Overrides previous paths, if present.\n"
        "XCI options:\n"
        "  --rootdir=dir      Specify XCI root HFS0 directory path.\n"
        "  --updatedir=dir    Specify XCI update HFS0 directory path.\n"
        "  --normaldir=dir    Specify XCI normal HFS0 directory path.\n"
        "  --securedir=dir    Specify XCI secure HFS0 directory path.\n"
        "  --outdir=dir       Specify XCI directory path. Overrides previous paths, if present.\n"
        "\n", __TIME__, __DATE__, prog_name);
    exit(EXIT_FAILURE);
}

static int ishex(char c) {
    if ('a' <= c && c <= 'f') return 1;
    if ('A' <= c && c <= 'F') return 1;
    if ('0' <= c && c <= '9') return 1;
    return 0;
}

static char hextoi(char c) {
    if ('a' <= c && c <= 'f') return c - 'a' + 0xA;
    if ('A' <= c && c <= 'F') return c - 'A' + 0xA;
    if ('0' <= c && c <= '9') return c - '0';
    return 0;
}

void parse_hex_key(unsigned char *key, const char *hex) {
    if (strlen(hex) != 32) {
        fprintf(stderr, "Key must be 32 hex digits!\n");
        usage();
    }

    for (unsigned int i = 0; i < 32; i++) {
        if (!ishex(hex[i])) {
            fprintf(stderr, "Key must be 32 hex digits!\n");
            usage();
        }
    }

    memset(key, 0, 16);

    for (unsigned int i = 0; i < 32; i++) {
        char val = hextoi(hex[i]);
        if ((i & 1) == 0) {
            val <<= 4;
        }
        key[i >> 1] |= val;
    }
}

int main(int argc, char **argv) {
    hactool_ctx_t tool_ctx;
    hactool_ctx_t base_ctx; /* Context for base NCA, if used. */
    nca_ctx_t nca_ctx;
    char input_name[0x200];

    prog_name = (argc < 1) ? "hactool" : argv[0];

    nca_init(&nca_ctx);
    memset(&tool_ctx, 0, sizeof(tool_ctx));
    memset(&base_ctx, 0, sizeof(base_ctx));
    memset(input_name, 0, sizeof(input_name));
    nca_ctx.tool_ctx = &tool_ctx;
    
    nca_ctx.tool_ctx->file_type = FILETYPE_NCA;
    base_ctx.file_type = FILETYPE_NCA;

    nca_ctx.tool_ctx->action = ACTION_INFO | ACTION_EXTRACT;
    pki_initialize_keyset(&tool_ctx.settings.keyset, KEYSET_RETAIL);

    while (1) {
        int option_index;
        char c;
        static struct option long_options[] = 
        {
            {"extract", 0, NULL, 'x'},
            {"info", 0, NULL, 'i'},
            {"dev", 0, NULL, 'd'},
            {"verify", 0, NULL, 'y'},
            {"raw", 0, NULL, 'r'},
            {"intype", 1, NULL, 't'},
            {"section0", 1, NULL, 0},
            {"section1", 1, NULL, 1},
            {"section2", 1, NULL, 2},
            {"section3", 1, NULL, 3},
            {"section0dir", 1, NULL, 4},
            {"section1dir", 1, NULL, 5},
            {"section2dir", 1, NULL, 6},
            {"section3dir", 1, NULL, 7},
            {"exefs", 1, NULL, 8},
            {"romfs", 1, NULL, 9},
            {"exefsdir", 1, NULL, 10},
            {"romfsdir", 1, NULL, 11},
            {"titlekey", 1, NULL, 12},
            {"contentkey", 1, NULL, 13},
            {"listromfs", 0, NULL, 14},
            {"baseromfs", 1, NULL, 15},
            {"basenca", 1, NULL, 16},
            {"outdir", 1, NULL, 17},
            {"plaintext", 1, NULL, 18},
            {"header", 1, NULL, 19},
            {"pfs0dir", 1, NULL, 20},
            {"hfs0dir", 1, NULL, 21},
            {"rootdir", 1, NULL, 22},
            {"updatedir", 1, NULL, 23},
            {"normaldir", 1, NULL, 24},
            {"securedir", 1, NULL, 25},
            {NULL, 0, NULL, 0},
        };

        c = getopt_long(argc, argv, "dryxt:i", long_options, &option_index);
        if (c == -1)
            break;

        switch (c) 
        {
            case 'i':
                nca_ctx.tool_ctx->action |= ACTION_INFO;
                break;
            case 'x':
                nca_ctx.tool_ctx->action |= ACTION_EXTRACT;
                break;
            case 'y':
                nca_ctx.tool_ctx->action |= ACTION_VERIFY;
                break;
            case 'r':
                nca_ctx.tool_ctx->action |= ACTION_RAW;
                break;
            case 'd':
                pki_initialize_keyset(&tool_ctx.settings.keyset, KEYSET_DEV);
                break;
            case 't':
                if (!strcmp(optarg, "nca")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_NCA;
                } else if (!strcmp(optarg, "pfs0") || !strcmp(optarg, "exefs")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_PFS0;
                } else if (!strcmp(optarg, "romfs")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_ROMFS; 
                } else if (!strcmp(optarg, "hfs0")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_HFS0;
                } else if (!strcmp(optarg, "xci") || !strcmp(optarg, "gamecard") || !strcmp(optarg, "gc")) {
                    nca_ctx.tool_ctx->file_type = FILETYPE_XCI;
                }
                /* } else if (!strcmp(optarg, "package2") || !strcmp(optarg, "pk21")) {
                 *    nca_ctx.tool_ctx->file_type = FILETYPE_PACKAGE2;
                 * }
                 * } else if (!strcmp(optarg, "package1") || !strcmp(optarg, "pk11")) {
                 *    nca_ctx.tool_ctx->file_type = FILETYPE_PACKAGE1;
                 * }
                 */
                break;

            case 0: filepath_set(&nca_ctx.tool_ctx->settings.section_paths[0], optarg); break;
            case 1: filepath_set(&nca_ctx.tool_ctx->settings.section_paths[1], optarg); break;
            case 2: filepath_set(&nca_ctx.tool_ctx->settings.section_paths[2], optarg); break;
            case 3: filepath_set(&nca_ctx.tool_ctx->settings.section_paths[3], optarg); break;
            case 4: filepath_set(&nca_ctx.tool_ctx->settings.section_dir_paths[0], optarg); break;
            case 5: filepath_set(&nca_ctx.tool_ctx->settings.section_dir_paths[1], optarg); break;
            case 6: filepath_set(&nca_ctx.tool_ctx->settings.section_dir_paths[2], optarg); break;
            case 7: filepath_set(&nca_ctx.tool_ctx->settings.section_dir_paths[3], optarg); break;
            case 8:
                nca_ctx.tool_ctx->settings.exefs_path.enabled = 1;
                filepath_set(&nca_ctx.tool_ctx->settings.exefs_path.path, optarg); 
                break;
            case 9:
                nca_ctx.tool_ctx->settings.romfs_path.enabled = 1;
                filepath_set(&nca_ctx.tool_ctx->settings.romfs_path.path, optarg); 
                break;
            case 10:
                nca_ctx.tool_ctx->settings.exefs_dir_path.enabled = 1;
                filepath_set(&nca_ctx.tool_ctx->settings.exefs_dir_path.path, optarg); 
                break;
            case 11:
                nca_ctx.tool_ctx->settings.romfs_dir_path.enabled = 1;
                filepath_set(&nca_ctx.tool_ctx->settings.romfs_dir_path.path, optarg); 
                break;
            case 12:
                parse_hex_key(nca_ctx.tool_ctx->settings.titlekey, optarg);
                nca_ctx.tool_ctx->settings.has_titlekey = 1;
                break;
            case 13:
                parse_hex_key(nca_ctx.tool_ctx->settings.contentkey, optarg);
                nca_ctx.tool_ctx->settings.has_contentkey = 1;
                break;
            case 14:
                nca_ctx.tool_ctx->action |= ACTION_LISTROMFS;
                break;
            case 15:
                if (nca_ctx.tool_ctx->base_file != NULL) {
                    usage();
                    return EXIT_FAILURE;
                }
                if ((nca_ctx.tool_ctx->base_file = fopen(optarg, "rb")) == NULL) {
                    fprintf(stderr, "unable to open %s: %s\n", optarg, strerror(errno));
                    return EXIT_FAILURE;
                }
                nca_ctx.tool_ctx->base_file_type = BASEFILE_ROMFS;
                break;
            case 16:
                if (nca_ctx.tool_ctx->base_file != NULL) {
                    usage();
                    return EXIT_FAILURE;
                }
                if ((nca_ctx.tool_ctx->base_file = fopen(optarg, "rb")) == NULL) {
                    fprintf(stderr, "unable to open %s: %s\n", optarg, strerror(errno));
                    return EXIT_FAILURE;
                }
                nca_ctx.tool_ctx->base_file_type = BASEFILE_NCA;
                nca_ctx.tool_ctx->base_nca_ctx = malloc(sizeof(*nca_ctx.tool_ctx->base_nca_ctx));
                if (nca_ctx.tool_ctx->base_nca_ctx == NULL) {
                    fprintf(stderr, "Failed to allocate base NCA context!\n");
                    return EXIT_FAILURE;
                }
                nca_init(nca_ctx.tool_ctx->base_nca_ctx);
                base_ctx.file = nca_ctx.tool_ctx->base_file;
                nca_ctx.tool_ctx->base_nca_ctx->file = base_ctx.file;
                break;
            case 17:
                tool_ctx.settings.out_dir_path.enabled = 1;
                filepath_set(&tool_ctx.settings.out_dir_path.path, optarg); 
                break;
            case 18:
                filepath_set(&nca_ctx.tool_ctx->settings.dec_nca_path, optarg); 
                break;
            case 19:
                filepath_set(&nca_ctx.tool_ctx->settings.header_path, optarg); 
                break;
            case 20:
                filepath_set(&tool_ctx.settings.pfs0_dir_path, optarg); 
                break;
            case 21:
                filepath_set(&tool_ctx.settings.hfs0_dir_path, optarg); 
                break;
            case 22:
                filepath_set(&tool_ctx.settings.rootpt_dir_path, optarg); 
                break;
            case 23:
                filepath_set(&tool_ctx.settings.update_dir_path, optarg); 
                break;
            case 24:
                filepath_set(&tool_ctx.settings.normal_dir_path, optarg); 
                break;
            case 25:
                filepath_set(&tool_ctx.settings.secure_dir_path, optarg); 
                break;
            default:
                usage();
                return EXIT_FAILURE;
        }
    }

    if (optind == argc - 1) {
        /* Copy input file. */
        strncpy(input_name, argv[optind], sizeof(input_name));
    } else if ((optind < argc) || (argc == 1)) {
        usage();
    }

    if ((tool_ctx.file = fopen(input_name, "rb")) == NULL) {
        fprintf(stderr, "unable to open %s: %s\n", input_name, strerror(errno));
        return EXIT_FAILURE;
    }
    
    switch (tool_ctx.file_type) {
        case FILETYPE_NCA: {
            if (nca_ctx.tool_ctx->base_nca_ctx != NULL) {
                memcpy(&base_ctx.settings.keyset, &tool_ctx.settings.keyset, sizeof(nca_keyset_t));
                nca_ctx.tool_ctx->base_nca_ctx->tool_ctx = &base_ctx;
                nca_process(nca_ctx.tool_ctx->base_nca_ctx);
                int found_romfs = 0;
                for (unsigned int i = 0; i < 4; i++) {
                    if (nca_ctx.tool_ctx->base_nca_ctx->section_contexts[i].is_present && nca_ctx.tool_ctx->base_nca_ctx->section_contexts[i].type == ROMFS) {
                        found_romfs = 1;
                        break;
                    }
                }
                if (found_romfs == 0) {
                    fprintf(stderr, "Unable to locate RomFS in base NCA!\n");
                    return EXIT_FAILURE;
                }
            }

            nca_ctx.file = tool_ctx.file;
            nca_process(&nca_ctx);
            nca_free_section_contexts(&nca_ctx);
            
            if (nca_ctx.tool_ctx->base_file != NULL) {
                fclose(nca_ctx.tool_ctx->base_file);
                if (nca_ctx.tool_ctx->base_file_type == BASEFILE_NCA) {
                    nca_free_section_contexts(nca_ctx.tool_ctx->base_nca_ctx);
                    free(nca_ctx.tool_ctx->base_nca_ctx);
                }
            }     
            break;
        }
        case FILETYPE_PFS0: {
            pfs0_ctx_t pfs0_ctx;
            memset(&pfs0_ctx, 0, sizeof(pfs0_ctx));
            pfs0_ctx.file = tool_ctx.file;
            pfs0_ctx.tool_ctx = &tool_ctx;
            pfs0_process(&pfs0_ctx);
            if (pfs0_ctx.header) {
                free(pfs0_ctx.header);
            }
            if (pfs0_ctx.npdm) {
                free(pfs0_ctx.npdm);
            }
            break;
        }
        case FILETYPE_ROMFS: {
            romfs_ctx_t romfs_ctx;
            memset(&romfs_ctx, 0, sizeof(romfs_ctx));
            romfs_ctx.file = tool_ctx.file;
            romfs_ctx.tool_ctx = &tool_ctx;
            romfs_process(&romfs_ctx);
            if (romfs_ctx.files) {
                free(romfs_ctx.files);
            }
            if (romfs_ctx.directories) {
                free(romfs_ctx.directories);
            }
            break;
        }
        case FILETYPE_HFS0: {
            hfs0_ctx_t hfs0_ctx;
            memset(&hfs0_ctx, 0, sizeof(hfs0_ctx));
            hfs0_ctx.file = tool_ctx.file;
            hfs0_ctx.tool_ctx = &tool_ctx;
            hfs0_process(&hfs0_ctx);
            if (hfs0_ctx.header) {
                free(hfs0_ctx.header);
            }
            break;
        }
        case FILETYPE_XCI: {
            xci_ctx_t xci_ctx;
            memset(&xci_ctx, 0, sizeof(xci_ctx));
            xci_ctx.file = tool_ctx.file;
            xci_ctx.tool_ctx = &tool_ctx;
            xci_process(&xci_ctx);
            break;
        }
        default: {
            fprintf(stderr, "Unknown File Type!\n\n");
            usage();
        }
    }
    
    if (tool_ctx.file != NULL) {
        fclose(tool_ctx.file);
    }
    printf("Done!\n");

    return EXIT_SUCCESS;
}
