#include <stdlib.h>
#include "nca.h"
#include "aes.h"
#include "sha.h"
#include "rsa.h"
#include "utils.h"
#include "filepath.h"

/* Initialize the context. */
void nca_init(nca_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

/* Updates the CTR for an offset. */
void nca_update_ctr(unsigned char *ctr, uint64_t ofs) {
    ofs >>= 4;
    for (unsigned int j = 0; j < 0x8; j++) {
        ctr[0x10-j-1] = (unsigned char)(ofs & 0xFF);
        ofs >>= 8;
    }
}

/* Updates the CTR for a bktr offset. */
void nca_update_bktr_ctr(unsigned char *ctr, uint32_t ctr_val, uint64_t ofs) {
    ofs >>= 4;
    for (unsigned int j = 0; j < 0x8; j++) {
        ctr[0x10-j-1] = (unsigned char)(ofs & 0xFF);
        ofs >>= 8;
    }
    for (unsigned int j = 0; j < 4; j++) {
        ctr[0x8-j-1] = (unsigned char)(ctr_val & 0xFF);
        ctr_val >>= 8;
    }
}

/* Seek to an offset within a section. */
void nca_section_fseek(nca_section_ctx_t *ctx, uint64_t offset) {
    if (ctx->is_decrypted) {
        fseeko64(ctx->file, (ctx->offset + offset), SEEK_SET);
        ctx->cur_seek = (ctx->offset + offset);
    } else if (ctx->header->crypt_type == CRYPT_XTS) {
        fseeko64(ctx->file, (ctx->offset + offset) & ~0x1FF, SEEK_SET);
        ctx->cur_seek = (ctx->offset + offset) & ~0x1FF;
        ctx->sector_num = offset / 0x200;
        ctx->sector_ofs = offset & 0x1FF;
    } else if (ctx->type == BKTR && ctx->bktr_ctx.subsection_block != NULL) { 
        /* No better way to do this than to make all BKTR seeking virtual. */
        ctx->bktr_ctx.virtual_seek = offset;
        if (ctx->tool_ctx->base_file == NULL && ctx->physical_reads == 0) { /* Without base romfs, reads will be physical. */
            ctx->bktr_ctx.bktr_seek = offset;
        } else { /* Let's do the complicated thing. */
            bktr_relocation_entry_t *reloc = bktr_get_relocation(ctx->bktr_ctx.relocation_block, offset);
            uint64_t section_ofs = offset - reloc->virt_offset + reloc->phys_offset;
            if (reloc->is_patch) {
                /* Seeked within the patch romfs. */
                ctx->bktr_ctx.bktr_seek = section_ofs;
            } else {
                /* Seeked within the base romfs. */
                ctx->bktr_ctx.base_seek = section_ofs;
            }
        }
    } else if (ctx->header->crypt_type != CRYPT_NONE) { /* CTR, and BKTR until subsections are read. */
        fseeko64(ctx->file, (ctx->offset + offset) & ~0xF, SEEK_SET);
        ctx->cur_seek = (ctx->offset + offset) & ~0xF;
        nca_update_ctr(ctx->ctr, ctx->offset + offset);
        ctx->sector_ofs = offset & 0xF;
    }
}

size_t nca_bktr_section_physical_fread(nca_section_ctx_t *ctx, void *buffer, size_t count) {
    size_t read = 0; /* XXX */
    size_t size = 1;
    char block_buf[0x10];
    
    if (ctx->is_decrypted) {
        fseeko64(ctx->file, (ctx->offset + ctx->bktr_ctx.bktr_seek), SEEK_SET);
        read = fread(buffer, size, count, ctx->file);
        nca_section_fseek(ctx, ctx->bktr_ctx.virtual_seek + read);
        return read;
    }
    
    bktr_subsection_entry_t *subsec = bktr_get_subsection(ctx->bktr_ctx.subsection_block, ctx->bktr_ctx.bktr_seek);
    nca_update_bktr_ctr(ctx->ctr, subsec->ctr_val, ctx->bktr_ctx.bktr_seek + ctx->offset);
    fseeko64(ctx->file, (ctx->offset + ctx->bktr_ctx.bktr_seek) & ~0xF, SEEK_SET);
    uint32_t block_ofs;
    bktr_subsection_entry_t *next_subsec = bktr_get_subsection(ctx->bktr_ctx.subsection_block, ctx->bktr_ctx.bktr_seek + count);
    if (next_subsec == subsec || (ctx->bktr_ctx.bktr_seek + count == next_subsec->offset && next_subsec == subsec + 1)) {
        /* Easy path, reading *only* within the subsection. */
        if ((block_ofs = ctx->bktr_ctx.bktr_seek & 0xF) != 0) {
            if ((read = fread(block_buf, 1, 0x10, ctx->file)) != 0x10) {
                return 0;
            }
            aes_setiv(ctx->aes, ctx->ctr, 0x10);
            aes_decrypt(ctx->aes, block_buf, block_buf, 0x10);
            if (count + block_ofs < 0x10) {
                memcpy(buffer, block_buf + ctx->sector_ofs, count);
                nca_section_fseek(ctx, ctx->bktr_ctx.virtual_seek + count);
                return count;
            }
            memcpy(buffer, block_buf + block_ofs, 0x10 - block_ofs);
            uint32_t read_in_block = 0x10 - block_ofs;
            nca_section_fseek(ctx, ctx->bktr_ctx.virtual_seek - block_ofs + 0x10);
            return read_in_block + nca_section_fread(ctx, (char *)buffer + read_in_block, count - read_in_block);
        }
        if ((read = fread(buffer, 1, count, ctx->file)) != count) {
                return 0;
        }                  
        aes_setiv(ctx->aes, ctx->ctr, 16);
        aes_decrypt(ctx->aes, buffer, buffer, count);
        nca_section_fseek(ctx, ctx->bktr_ctx.virtual_seek + count);
    } else {
        /* Sad path. */
        uint64_t within_subsection = next_subsec->offset - ctx->bktr_ctx.bktr_seek;
        if ((read = nca_section_fread(ctx, buffer, within_subsection)) != within_subsection) {
            return 0;
        }
        read += nca_section_fread(ctx, (char *)buffer + within_subsection, count - within_subsection);
        if (read != count) {
            return 0;
        }
    }
    
    return read;
}

size_t nca_section_fread(nca_section_ctx_t *ctx, void *buffer, size_t count) {
    size_t read = 0; /* XXX */
    size_t size = 1;
    char block_buf[0x10];

    if (ctx->is_decrypted && ctx->type != BKTR) {
        read = fread(buffer, size, count, ctx->file);
        return read;
    }

    if (ctx->header->crypt_type == CRYPT_XTS) { /* AES-XTS requires special handling... */
        unsigned char sector_buf[0x200];
        if ((read = fread(&sector_buf, size, 0x200, ctx->file)) != 0x200) {
            return 0;
        }
        aes_xts_decrypt(ctx->aes, &sector_buf, &sector_buf, 0x200, ctx->sector_num, 0x200);
        if (count > 0x200 - ctx->sector_ofs) { /* We're leaving the sector... */
            memcpy(buffer, &sector_buf + ctx->sector_ofs, 0x200 - ctx->sector_ofs);
            ctx->sector_num++;
            ctx->sector_ofs = 0;
            size_t remaining = count - (0x200 - ctx->sector_ofs);
            size_t ofs = (0x200 - ctx->sector_ofs);
            if (remaining & ~0x1FF) { /* Read intermediate sectors. */
                if ((read = fread((char *)buffer + ofs, size, (remaining & ~0x1FF), ctx->file)) != (remaining & ~0x1FF)) {
                    return ofs;
                }

                aes_xts_decrypt(ctx->aes, (char *)buffer + ofs, (char *)buffer + ofs, remaining & ~0x1FF, ctx->sector_num, 0x200);
                ctx->sector_num += remaining / 0x200;
                ofs += remaining & ~0x1FF;
                remaining &= 0x1FF;
            }
            if (remaining) { /* Read last sector. */
                if ((read = fread(&sector_buf, size, 0x200, ctx->file)) != 0x200) {
                    return ofs;
                }
                aes_xts_decrypt(ctx->aes, &sector_buf, &sector_buf, 0x200, ctx->sector_num, 0x200);
                memcpy((char *)buffer + ofs, &sector_buf, remaining);
                ctx->sector_ofs = remaining;
                read = count;
            }
        } else {
            memcpy(buffer, &sector_buf + ctx->sector_ofs, count);
            ctx->sector_num += (ctx->sector_ofs + count) / 0x200;
            ctx->sector_ofs += count;
            ctx->sector_ofs &= 0x1FF;
        }
    } else {
        /* Perform decryption, if necessary. */
        /* AES-CTR. */
        if (ctx->header->crypt_type == CRYPT_CTR || (ctx->header->crypt_type == CRYPT_BKTR && ctx->bktr_ctx.subsection_block == NULL))
        {                
            if (ctx->sector_ofs) {
                if ((read = fread(block_buf, 1, 0x10, ctx->file)) != 0x10) {
                    return 0;
                }
                aes_setiv(ctx->aes, ctx->ctr, 0x10);
                aes_decrypt(ctx->aes, block_buf, block_buf, 0x10);
                if (count + ctx->sector_ofs < 0x10) {
                    memcpy(buffer, block_buf + ctx->sector_ofs, count);
                    ctx->sector_ofs += count;
                    nca_section_fseek(ctx, ctx->cur_seek - ctx->offset);
                    return count;
                }
                memcpy(buffer, block_buf + ctx->sector_ofs, 0x10 - ctx->sector_ofs);
                uint32_t read_in_block = 0x10 - ctx->sector_ofs;
                nca_section_fseek(ctx, ctx->cur_seek - ctx->offset + 0x10);
                return read_in_block + nca_section_fread(ctx, (char *)buffer + read_in_block, count - read_in_block);
            }
            if ((read = fread(buffer, 1, count, ctx->file)) != count) {
                    return 0;
            }                  
            aes_setiv(ctx->aes, ctx->ctr, 16);
            aes_decrypt(ctx->aes, buffer, buffer, count);
            nca_section_fseek(ctx, ctx->cur_seek - ctx->offset + count);
        } else if (ctx->header->crypt_type == CRYPT_BKTR) { /* Spooky BKTR AES-CTR. */
            /* Are we doing virtual reads, or physical reads? */
            if (ctx->tool_ctx->base_file != NULL && ctx->physical_reads == 0) {
                bktr_relocation_entry_t *reloc = bktr_get_relocation(ctx->bktr_ctx.relocation_block, ctx->bktr_ctx.virtual_seek);
                bktr_relocation_entry_t *next_reloc = reloc + 1;
                uint64_t virt_seek = ctx->bktr_ctx.virtual_seek;
                if (ctx->bktr_ctx.virtual_seek + count <= next_reloc->virt_offset) {
                    /* Easy path: We're reading *only* within the current relocation. */
                    if (reloc->is_patch) {
                        read = nca_bktr_section_physical_fread(ctx, buffer, count);
                    } else {
                        /* Nice and easy read from the base rom. */
                        if (ctx->tool_ctx->base_file_type == BASEFILE_ROMFS) {
                            fseeko64(ctx->tool_ctx->base_file, ctx->bktr_ctx.base_seek, SEEK_SET);
                            if ((read = fread(buffer, 1, count, ctx->tool_ctx->base_file)) != count) {
                                return 0;
                            }
                        } else {
                            nca_ctx_t *base_ctx = ctx->tool_ctx->base_nca_ctx;
                            unsigned int romfs_section_num;
                            for (romfs_section_num = 0; romfs_section_num < 4; romfs_section_num++) {
                                if (base_ctx->section_contexts[romfs_section_num].type == ROMFS) {
                                    break;
                                }
                            }
                            nca_section_fseek(&base_ctx->section_contexts[romfs_section_num], ctx->bktr_ctx.base_seek);
                            if ((read = nca_section_fread(&base_ctx->section_contexts[romfs_section_num], buffer, count)) != count) {
                                fprintf(stderr, "Failed to read from Base NCA RomFS!\n");
                                exit(EXIT_FAILURE);
                            }
                        }
                    }        
                } else {
                    uint64_t within_relocation = next_reloc->virt_offset - ctx->bktr_ctx.virtual_seek;
                    if ((read = nca_section_fread(ctx, buffer, within_relocation)) != within_relocation) {
                        return 0;
                    }
                    nca_section_fseek(ctx, virt_seek + within_relocation);
                    read += nca_section_fread(ctx, (char *)buffer + within_relocation, count - within_relocation);
                    if (read != count) {
                        return 0;
                    }
                }
                nca_section_fseek(ctx, virt_seek + count);
            } else {
                read = nca_bktr_section_physical_fread(ctx, buffer, count);
            }
        }
    }
    return read;
}

void nca_free_section_contexts(nca_ctx_t *ctx) {
    for (unsigned int i = 0; i < 4; i++) {
        if (ctx->section_contexts[i].is_present) {
            if (ctx->section_contexts[i].aes) {
                free_aes_ctx(ctx->section_contexts[i].aes);
            }
            if (ctx->section_contexts[i].pfs0_ctx.is_exefs) {
                free(ctx->section_contexts[i].pfs0_ctx.npdm);
            } else if (ctx->section_contexts[i].type == ROMFS) {
                if (ctx->section_contexts[i].romfs_ctx.directories) {
                    free(ctx->section_contexts[i].romfs_ctx.directories);
                }
                if (ctx->section_contexts[i].romfs_ctx.files) {
                    free(ctx->section_contexts[i].romfs_ctx.files);
                }
            } else if (ctx->section_contexts[i].type == BKTR) {
                if (ctx->section_contexts[i].bktr_ctx.subsection_block) {
                    free(ctx->section_contexts[i].bktr_ctx.subsection_block);
                }
                if (ctx->section_contexts[i].bktr_ctx.relocation_block) {
                    free(ctx->section_contexts[i].bktr_ctx.relocation_block);
                }
                if (ctx->section_contexts[i].bktr_ctx.directories) {
                    free(ctx->section_contexts[i].bktr_ctx.directories);
                }
                if (ctx->section_contexts[i].bktr_ctx.files) {
                    free(ctx->section_contexts[i].bktr_ctx.files);
                }
            }
        }
    }
}

void nca_save(nca_ctx_t *ctx) {
    /* Save header. */
    filepath_t *header_path = &ctx->tool_ctx->settings.header_path;

    if (header_path->valid == VALIDITY_VALID) {
        printf("Saving Header to %s...\n", header_path->char_path);
        FILE *f_hdr = os_fopen(header_path->os_path, OS_MODE_WRITE);

        if (f_hdr != NULL) {
            fwrite(&ctx->header, 1, 0xC00, f_hdr);
            fclose(f_hdr);
        } else {
            fprintf(stderr, "Failed to open %s!\n", header_path->char_path);
        }
    }

    
    for (unsigned int i = 0; i < 4; i++) {
        if (ctx->section_contexts[i].is_present) {
            /* printf("Saving section %"PRId32"...\n", i); */
            nca_save_section(&ctx->section_contexts[i]);
            printf("\n");
        }
    }
    
    /* Save Decrypted NCA. */
    filepath_t *dec_path = &ctx->tool_ctx->settings.dec_nca_path;

    if (dec_path->valid == VALIDITY_VALID) {
        printf("Saving Decrypted NCA to %s...\n", dec_path->char_path);
        FILE *f_dec = os_fopen(dec_path->os_path, OS_MODE_WRITE);

        if (f_dec != NULL) {
            if (fwrite(&ctx->header, 1, 0xC00, f_dec) != 0xC00) {
                fprintf(stderr, "Failed to write header!\n");
                exit(EXIT_FAILURE);
            }
            
            unsigned char *buf = malloc(0x400000);
            if (buf == NULL) {
                fprintf(stderr, "Failed to allocate file-save buffer!\n");
                exit(EXIT_FAILURE);
            }
            for (unsigned int i = 0; i < 4; i++) {
                if (ctx->section_contexts[i].is_present) {
                    fseeko64(f_dec, ctx->section_contexts[i].offset, SEEK_SET);
                    ctx->section_contexts[i].physical_reads = 1;
                    
                    uint64_t read_size = 0x400000; /* 4 MB buffer. */
                    memset(buf, 0xCC, read_size); /* Debug in case I fuck this up somehow... */
                    uint64_t ofs = 0;
                    uint64_t end_ofs = ofs + ctx->section_contexts[i].size;
                    nca_section_fseek(&ctx->section_contexts[i], ofs);
                    while (ofs < end_ofs) {       
                        if (ofs + read_size >= end_ofs) read_size = end_ofs - ofs;
                        if (nca_section_fread(&ctx->section_contexts[i], buf, read_size) != read_size) {
                            fprintf(stderr, "Failed to read file!\n");
                            exit(EXIT_FAILURE);
                        }
                        if (fwrite(buf, 1, read_size, f_dec) != read_size) {
                            fprintf(stderr, "Failed to write file!\n");
                            exit(EXIT_FAILURE);
                        }
                        ofs += read_size;
                    }

                    ctx->section_contexts[i].physical_reads = 0;
                }
            }
            
            fclose(f_dec);

            free(buf);
        } else {
            fprintf(stderr, "Failed to open %s!\n", dec_path->char_path);
        }
    }
}

void nca_process(nca_ctx_t *ctx) {
    /* First things first, decrypt header. */
    if (!nca_decrypt_header(ctx)) {
        fprintf(stderr, "Invalid NCA header!\n");
        return;
    }

    if (rsa2048_pss_verify(&ctx->header.magic, 0x200, ctx->header.fixed_key_sig, ctx->tool_ctx->settings.keyset.nca_hdr_fixed_key_modulus)) {
        ctx->fixed_sig_validity = VALIDITY_VALID;
    } else {
        ctx->fixed_sig_validity = VALIDITY_INVALID;
    }

    /* Sort out crypto type. */
    ctx->crypto_type = ctx->header.crypto_type;
    if (ctx->header.crypto_type2 > ctx->header.crypto_type)
        ctx->crypto_type = ctx->header.crypto_type2;

    if (ctx->crypto_type)
        ctx->crypto_type--; /* 0, 1 are both master key 0. */

    /* Rights ID. */
    for (unsigned int i = 0; i < 0x10; i++) {
        if (ctx->header.rights_id[i] != 0) {
            ctx->has_rights_id = 1;
            break;
        }
    }

    /* Decrypt key area if required. */
    if (!ctx->has_rights_id) {
        nca_decrypt_key_area(ctx);
    } else {
        /* Decrypt title key. */
        if (ctx->tool_ctx->settings.has_titlekey) {
            aes_ctx_t *aes_ctx = new_aes_ctx(ctx->tool_ctx->settings.keyset.titlekeks[ctx->crypto_type], 16, AES_MODE_CTR);
            aes_decrypt(aes_ctx, ctx->tool_ctx->settings.dec_titlekey, ctx->tool_ctx->settings.titlekey, 0x10);
            free_aes_ctx(aes_ctx);
        }
    }

    /* Parse sections. */
    for (unsigned int i = 0; i < 4; i++) {
        if (ctx->header.section_entries[i].media_start_offset) { /* Section exists. */
            ctx->section_contexts[i].is_present = 1;
            ctx->section_contexts[i].is_decrypted = ctx->is_decrypted;
            ctx->section_contexts[i].tool_ctx = ctx->tool_ctx;
            ctx->section_contexts[i].file = ctx->file;
            ctx->section_contexts[i].section_num = i;
            ctx->section_contexts[i].offset = media_to_real(ctx->header.section_entries[i].media_start_offset);
            ctx->section_contexts[i].size = media_to_real(ctx->header.section_entries[i].media_end_offset) - ctx->section_contexts[i].offset;
            ctx->section_contexts[i].header = &ctx->header.fs_headers[i];
            if (ctx->section_contexts[i].header->partition_type == PARTITION_PFS0 && ctx->section_contexts[i].header->fs_type == FS_TYPE_PFS0) {
                ctx->section_contexts[i].type = PFS0;
                ctx->section_contexts[i].pfs0_ctx.superblock = &ctx->section_contexts[i].header->pfs0_superblock;
            } else if (ctx->section_contexts[i].header->partition_type == PARTITION_ROMFS && ctx->section_contexts[i].header->fs_type == FS_TYPE_ROMFS) {
                if (ctx->section_contexts[i].header->crypt_type == CRYPT_BKTR) {
                    ctx->section_contexts[i].type = BKTR;
                    ctx->section_contexts[i].bktr_ctx.superblock = &ctx->section_contexts[i].header->bktr_superblock;
                } else {
                    ctx->section_contexts[i].type = ROMFS;
                    ctx->section_contexts[i].romfs_ctx.superblock = &ctx->section_contexts[i].header->romfs_superblock;
                }
            } else {
                ctx->section_contexts[i].type = INVALID;
            }
            uint64_t ofs = ctx->section_contexts[i].offset >> 4;
            for (unsigned int j = 0; j < 0x8; j++) {
                ctx->section_contexts[i].ctr[j] = ctx->section_contexts[i].header->section_ctr[0x8-j-1];
                ctx->section_contexts[i].ctr[0x10-j-1] = (unsigned char)(ofs & 0xFF);
                ofs >>= 8;
            }
            ctx->section_contexts[i].sector_num = 0;
            ctx->section_contexts[i].sector_ofs = 0;

            if (ctx->section_contexts[i].header->crypt_type == CRYPT_NONE) {
                ctx->section_contexts[i].is_decrypted = 1;
            }

            if (ctx->tool_ctx->settings.has_contentkey) {
                ctx->section_contexts[i].aes = new_aes_ctx(ctx->tool_ctx->settings.contentkey, 16, AES_MODE_CTR);
            } else {
                if (ctx->has_rights_id) {
                    ctx->section_contexts[i].aes = new_aes_ctx(ctx->tool_ctx->settings.dec_titlekey, 16, AES_MODE_CTR);
                } else {
                    if (ctx->section_contexts[i].header->crypt_type == CRYPT_CTR || ctx->section_contexts[i].header->crypt_type == CRYPT_BKTR) {
                        ctx->section_contexts[i].aes = new_aes_ctx(ctx->decrypted_keys[2], 16, AES_MODE_CTR);
                    } else if (ctx->section_contexts[i].header->crypt_type == CRYPT_XTS) {
                        ctx->section_contexts[i].aes = new_aes_ctx(ctx->decrypted_keys[0], 32, AES_MODE_XTS);
                    }
                }
            }

            if (ctx->tool_ctx->action & ACTION_VERIFY) {
                printf("Verifying section %"PRId32"...\n", i);
            }

            switch (ctx->section_contexts[i].type) {
                case PFS0:
                    nca_process_pfs0_section(&ctx->section_contexts[i]);
                    /* Verify NPDM sig now, if we can... */
                    if (ctx->section_contexts[i].pfs0_ctx.is_exefs) {
                        ctx->npdm = ctx->section_contexts[i].pfs0_ctx.npdm;
                        if (rsa2048_pss_verify(&ctx->header.magic, 0x200, ctx->header.npdm_key_sig, npdm_get_acid(ctx->npdm)->modulus)) {
                            ctx->npdm_sig_validity = VALIDITY_VALID;
                        } else {
                            ctx->npdm_sig_validity = VALIDITY_INVALID;
                        }
                    }
                    break;
                case ROMFS:
                    nca_process_ivfc_section(&ctx->section_contexts[i]);
                    break;
                case BKTR:
                    nca_process_bktr_section(&ctx->section_contexts[i]);
                    break;
                case INVALID:
                default:
                    break;
            }
        }
    }

    if (ctx->tool_ctx->action & ACTION_INFO) {
        nca_print(ctx);
    }

    if (ctx->tool_ctx->action & ACTION_EXTRACT) {
        nca_save(ctx);
    }
}

/* Decrypt NCA header. */
int nca_decrypt_header(nca_ctx_t *ctx) {
    fseeko64(ctx->file, 0, SEEK_SET);
    if (fread(&ctx->header, 1, 0xC00, ctx->file) != 0xC00) {
        fprintf(stderr, "Failed to read NCA header!\n");
        return 0;
    }

    /* Try to support decrypted NCA headers. */
    if (ctx->header.magic == MAGIC_NCA3) {
        if (ctx->header._0x340[0] == 0 && !memcmp(ctx->header._0x340, ctx->header._0x340 + 1, 0xBF)) {
            ctx->is_decrypted = 1;
            return 1;
        }
    }

    ctx->is_decrypted = 0;

    aes_ctx_t *aes_ctx = new_aes_ctx(ctx->tool_ctx->settings.keyset.header_key, 32, AES_MODE_XTS);
    aes_xts_decrypt(aes_ctx, &ctx->header, &ctx->header, 0xC00, 0, 0x200);
    free_aes_ctx(aes_ctx);
    return ctx->header.magic == MAGIC_NCA3;
}

/* Decrypt key area. */
void nca_decrypt_key_area(nca_ctx_t *ctx) {
    aes_ctx_t *aes_ctx = new_aes_ctx(ctx->tool_ctx->settings.keyset.key_area_keys[ctx->crypto_type][ctx->header.kaek_ind], 16, AES_MODE_ECB);
    aes_decrypt(aes_ctx, ctx->decrypted_keys, ctx->header.encrypted_keys, 0x40);
    free_aes_ctx(aes_ctx);
}


char *nca_get_distribution_type(nca_ctx_t *ctx) {
    switch (ctx->header.distribution) {
        case 0:
            return "Download";
        case 1:
            return "Gamecard";
        default:
            return "Unknown";
    }
}

char *nca_get_content_type(nca_ctx_t *ctx) {
    switch (ctx->header.content_type) {
        case 0:
            return "Program";
        case 1:
            return "Meta";
        case 2:
            return "Control";
        case 3:
            return "Manual";
        case 4:
            return "Data";
        default:
            return "Unknown";
    }
}

char *nca_get_master_key_summary(uint8_t master_key_rev) {
    switch (master_key_rev) {
        case 0:
            return "1.0.0-2.3.0";
        case 1:
            return "3.0.0";
        case 2:
            return "3.0.1-3.0.2";
        case 3:
            return "4.0.0-4.1.0";
        default:
            return "Unknown";
    }
}

char *nca_get_encryption_type(nca_ctx_t *ctx) {
    if (ctx->has_rights_id) {
        return "Titlekey crypto";
    } else {
        return "Standard crypto";
    }
}

void nca_print_key_area(nca_ctx_t *ctx) {
    printf("Key Area (Encrypted):\n");
    for (unsigned int i = 0; i < 0x4; i++) {
        printf("    Key %"PRId32" (Encrypted):              ", i);
        memdump(stdout, "", &ctx->header.encrypted_keys[i], 0x10);
    }
    printf("Key Area (Decrypted):\n");
    for (unsigned int i = 0; i < 0x4; i++) {
        printf("    Key %"PRId32" (Decrypted):              ", i);
        memdump(stdout, "", &ctx->decrypted_keys[i], 0x10);
    }
}

char *nca_get_section_type(nca_section_ctx_t *meta) {
    switch (meta->type) {
        case PFS0: {
            if (meta->pfs0_ctx.is_exefs) return "ExeFS";
            return "PFS0";
        }
        case ROMFS:     return "RomFS";
        case BKTR:      return "Patch RomFS";
        case INVALID:
        default:
            return "Unknown/Invalid";
    }
}


void nca_print_sections(nca_ctx_t *ctx) {
    printf("Sections:\n");
    for (unsigned int i = 0; i < 4; i++) {
        if (ctx->section_contexts[i].is_present) { /* Section exists. */            
            printf("    Section %"PRId32":\n", i);
            printf("        Offset:                     0x%012"PRIx64"\n", ctx->section_contexts[i].offset);
            printf("        Size:                       0x%012"PRIx64"\n", ctx->section_contexts[i].size);
            printf("        Partition Type:             %s\n", nca_get_section_type(&ctx->section_contexts[i]));
            memdump(stdout, "        Section CTR:                ", &ctx->section_contexts[i].ctr, 16);
            switch (ctx->section_contexts[i].type) {
                case PFS0:     {
                    nca_print_pfs0_section(&ctx->section_contexts[i]);
                    break;
                }
                case ROMFS:     {
                    nca_print_ivfc_section(&ctx->section_contexts[i]);
                    break;
                }
                case BKTR:     {
                    nca_print_bktr_section(&ctx->section_contexts[i]);
                    break;
                }
                case INVALID:
                default:     {
                    printf("        Unknown/invalid superblock!");
                }
            }
        }
    }

}

/* Print out information about the NCA. */
void nca_print(nca_ctx_t *ctx) {
    printf("\nNCA:\n");
    print_magic("Magic:                         ", ctx->header.magic);

    if (ctx->tool_ctx->action & ACTION_VERIFY && ctx->fixed_sig_validity != VALIDITY_UNCHECKED) {
        if (ctx->fixed_sig_validity == VALIDITY_VALID) {
            memdump(stdout, "Fixed-Key Signature (GOOD):         ", &ctx->header.fixed_key_sig, 0x100);
        } else {
            memdump(stdout, "Fixed-Key Signature (FAIL):         ", &ctx->header.fixed_key_sig, 0x100);
        }
    } else {
        memdump(stdout, "Fixed-Key Signature:                ", &ctx->header.fixed_key_sig, 0x100);
    }
    if (ctx->tool_ctx->action & ACTION_VERIFY && ctx->npdm_sig_validity != VALIDITY_UNCHECKED) {
        if (ctx->npdm_sig_validity == VALIDITY_VALID) {
            memdump(stdout, "NPDM Signature (GOOD):              ", &ctx->header.npdm_key_sig, 0x100);
        } else {
            memdump(stdout, "NPDM Signature (FAIL):              ", &ctx->header.npdm_key_sig, 0x100);
        }
    } else {
         memdump(stdout, "NPDM Signature:                     ", &ctx->header.npdm_key_sig, 0x100);
    }
    printf("Content Size:                       0x%012"PRIx64"\n", ctx->header.nca_size);
    printf("Title ID:                           %016"PRIx64"\n", ctx->header.title_id);
    printf("SDK Version:                        %"PRId8".%"PRId8".%"PRId8".%"PRId8"\n", ctx->header.sdk_major, ctx->header.sdk_minor, ctx->header.sdk_micro, ctx->header.sdk_revision);
    printf("Distribution type:                  %s\n", nca_get_distribution_type(ctx));
    printf("Content Type:                       %s\n", nca_get_content_type(ctx));
    printf("Master Key Revision:                %"PRIx8" (%s)\n", ctx->crypto_type, nca_get_master_key_summary(ctx->crypto_type));
    printf("Encryption Type:                    %s\n", nca_get_encryption_type(ctx));

    if (ctx->has_rights_id) {        
        memdump(stdout, "Rights ID:                          ", &ctx->header.rights_id, 0x10);
        if (ctx->tool_ctx->settings.has_titlekey) {
            memdump(stdout, "Titlekey (Encrypted)                ", ctx->tool_ctx->settings.titlekey, 0x10);
            memdump(stdout, "Titlekey (Decrypted)                ", ctx->tool_ctx->settings.dec_titlekey, 0x10);
        }
    } else {
        printf("Key Area Encryption Key:            %"PRIx8"\n", ctx->header.kaek_ind);
        nca_print_key_area(ctx);
    }

    if (ctx->npdm) {
        npdm_print(ctx->npdm, ctx->tool_ctx);
    }

    nca_print_sections(ctx);

    printf("\n");
}

validity_t nca_section_check_external_hash_table(nca_section_ctx_t *ctx, unsigned char *hash_table, uint64_t data_ofs, uint64_t data_len, uint64_t block_size, int full_block) {
    if (block_size == 0) {
        /* Block size of 0 is always invalid. */
        return VALIDITY_INVALID;
    }
    unsigned char cur_hash[0x20];
    uint64_t read_size = block_size;
    unsigned char *block = malloc(block_size);
    if (block == NULL) {
        fprintf(stderr, "Failed to allocate hash block!\n");
        exit(EXIT_FAILURE);
    }

    validity_t result = VALIDITY_VALID;
    unsigned char *cur_hash_table_entry = hash_table;
    for (uint64_t ofs = 0; ofs < data_len; ofs += read_size) {
        nca_section_fseek(ctx, ofs + data_ofs);
        if (ofs + read_size > data_len) {
            /* Last block... */
            memset(block, 0, read_size);
            read_size = data_len - ofs;
        }

        uint64_t r = nca_section_fread(ctx, block, read_size);
        if (r != read_size) {
            fprintf(stderr, "%012"PRIx64" %012"PRIx64" %08"PRIx64"\n", ofs, data_len, r);
            fprintf(stderr, "%d %d\n", ctx->is_decrypted, ctx->section_num);
            fprintf(stderr, "Failed to read section!\n");
            exit(EXIT_FAILURE);
        }        
        sha256_hash_buffer(cur_hash, block, full_block ? block_size : read_size);
        if (memcmp(cur_hash, cur_hash_table_entry, 0x20) != 0) {
            result = VALIDITY_INVALID;
            break;
        }
        cur_hash_table_entry += 0x20;
    }
    free(block);

    return result;

}

validity_t nca_section_check_hash_table(nca_section_ctx_t *ctx, uint64_t hash_ofs, uint64_t data_ofs, uint64_t data_len, uint64_t block_size, int full_block) {
    if (block_size == 0) {
        /* Block size of 0 is always invalid. */
        return VALIDITY_INVALID;
    }    
    uint64_t hash_table_size = data_len / block_size;
    if (data_len % block_size) hash_table_size++;
    hash_table_size *= 0x20;
    unsigned char *hash_table = malloc(hash_table_size);
    if (hash_table == NULL) {
        fprintf(stderr, "Failed to allocate hash table!\n");
        exit(EXIT_FAILURE);
    }

    nca_section_fseek(ctx, hash_ofs);
    if (nca_section_fread(ctx, hash_table, hash_table_size) != hash_table_size) {
        fprintf(stderr, "Failed to read section!\n");
        exit(EXIT_FAILURE);
    }

    validity_t result = nca_section_check_external_hash_table(ctx, hash_table, data_ofs, data_len, block_size, full_block);

    free(hash_table);

    return result;
}

void nca_save_pfs0_file(nca_section_ctx_t *ctx, uint32_t i, filepath_t *dirpath) {
    if (i >= ctx->pfs0_ctx.header->num_files) {
        fprintf(stderr, "Could not save file %"PRId32"!\n", i);
        exit(EXIT_FAILURE);
    }
    pfs0_file_entry_t *cur_file = pfs0_get_file_entry(ctx->pfs0_ctx.header, i);
    if (cur_file->size >= ctx->size) {
        fprintf(stderr, "File %"PRId32" too big in PFS0 (section %"PRId32")!\n", i, ctx->section_num);
        exit(EXIT_FAILURE);
    }

    if (strlen(pfs0_get_file_name(ctx->pfs0_ctx.header, i)) >= MAX_PATH - strlen(dirpath->char_path) - 1) {
        fprintf(stderr, "Filename too long in PFS0!\n");
        exit(EXIT_FAILURE);
    }

    filepath_t filepath;
    filepath_copy(&filepath, dirpath);
    filepath_append(&filepath, "%s", pfs0_get_file_name(ctx->pfs0_ctx.header, i));

    printf("Saving %s to %s...\n", pfs0_get_file_name(ctx->pfs0_ctx.header, i), filepath.char_path);
    uint64_t ofs = ctx->pfs0_ctx.superblock->pfs0_offset + pfs0_get_header_size(ctx->pfs0_ctx.header) + cur_file->offset;
    nca_save_section_file(ctx, ofs, cur_file->size, &filepath);
}


void nca_process_pfs0_section(nca_section_ctx_t *ctx) {
    pfs0_superblock_t *sb = ctx->pfs0_ctx.superblock;
    ctx->superblock_hash_validity = nca_section_check_external_hash_table(ctx, sb->master_hash, sb->hash_table_offset, sb->hash_table_size, sb->hash_table_size, 0);    
    if (ctx->tool_ctx->action & ACTION_VERIFY) {
        /* Verify actual PFS0... */
        ctx->pfs0_ctx.hash_table_validity = nca_section_check_hash_table(ctx, sb->hash_table_offset, sb->pfs0_offset, sb->pfs0_size, sb->block_size, 0);
    }

    if (ctx->superblock_hash_validity != VALIDITY_VALID) return;

    /* Read *just* safe amount. */
    pfs0_header_t raw_header; 
    nca_section_fseek(ctx, sb->pfs0_offset);
    if (nca_section_fread(ctx, &raw_header, sizeof(raw_header)) != sizeof(raw_header)) {
        fprintf(stderr, "Failed to read PFS0 header!\n");
        exit(EXIT_FAILURE);
    }

    uint64_t header_size = pfs0_get_header_size(&raw_header);
    ctx->pfs0_ctx.header = malloc(header_size);
    if (ctx->pfs0_ctx.header == NULL) {
        fprintf(stderr, "Failed to get PFS0 header size!\n");
        exit(EXIT_FAILURE);
    }
    nca_section_fseek(ctx, sb->pfs0_offset);
    if (nca_section_fread(ctx, ctx->pfs0_ctx.header, header_size) != header_size) {
        fprintf(stderr, "Failed to read PFS0 header!\n");
        exit(EXIT_FAILURE);
    }

    for (unsigned int i = 0; i < ctx->pfs0_ctx.header->num_files; i++) {
        pfs0_file_entry_t *cur_file = pfs0_get_file_entry(ctx->pfs0_ctx.header, i);
        if (strcmp(pfs0_get_file_name(ctx->pfs0_ctx.header, i), "main.npdm") == 0) {
            /* We might have found the exefs... */
            if (cur_file->size >= sb->pfs0_size) {
                fprintf(stderr, "NPDM too big!\n");
                exit(EXIT_FAILURE);
            }

            ctx->pfs0_ctx.npdm = malloc(cur_file->size);
            if (ctx->pfs0_ctx.npdm == NULL) {
                fprintf(stderr, "Failed to allocate NPDM!\n");
                exit(EXIT_FAILURE);
            }
            nca_section_fseek(ctx, sb->pfs0_offset + pfs0_get_header_size(ctx->pfs0_ctx.header) + cur_file->offset);
            if (nca_section_fread(ctx, ctx->pfs0_ctx.npdm, cur_file->size) != cur_file->size) {
                fprintf(stderr, "Failed to read NPDM!\n");
                exit(EXIT_FAILURE);
            }

            if (ctx->pfs0_ctx.npdm->magic == MAGIC_META) {
                ctx->pfs0_ctx.is_exefs = 1;
            }
        }
    }
}


void nca_process_ivfc_section(nca_section_ctx_t *ctx) {
    romfs_superblock_t *sb = ctx->romfs_ctx.superblock;
    for (unsigned int i = 0; i < IVFC_MAX_LEVEL; i++) {
        /* Load in the current level's header data. */
        ivfc_level_ctx_t *cur_level = &ctx->romfs_ctx.ivfc_levels[i];
        cur_level->data_offset = sb->ivfc_header.level_headers[i].logical_offset;
        cur_level->data_size = sb->ivfc_header.level_headers[i].hash_data_size;
        cur_level->hash_block_size = 1 << sb->ivfc_header.level_headers[i].block_size;

        if (i != 0) {
            /* Hash table is previous level's data. */
            cur_level->hash_offset = ctx->romfs_ctx.ivfc_levels[i-1].data_offset;
        } else {
            /* Hash table is the superblock hash. Always check the superblock hash. */
            ctx->superblock_hash_validity = nca_section_check_external_hash_table(ctx, sb->ivfc_header.master_hash, cur_level->data_offset, cur_level->data_size, cur_level->hash_block_size, 1);
            cur_level->hash_validity = ctx->superblock_hash_validity;
        }
        if (ctx->tool_ctx->action & ACTION_VERIFY && i != 0) {
            /* Actually check the table. */
            printf("    Verifying IVFC Level %"PRId32"...\n", i);
            cur_level->hash_validity = nca_section_check_hash_table(ctx, cur_level->hash_offset, cur_level->data_offset, cur_level->data_size, cur_level->hash_block_size, 1);
        }
    }

    ctx->romfs_ctx.romfs_offset = ctx->romfs_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1].data_offset;
    nca_section_fseek(ctx, ctx->romfs_ctx.romfs_offset);
    if (nca_section_fread(ctx, &ctx->romfs_ctx.header, sizeof(romfs_hdr_t)) != sizeof(romfs_hdr_t)) {
        fprintf(stderr, "Failed to read RomFS header!\n");
    }

    if ((ctx->tool_ctx->action & (ACTION_EXTRACT | ACTION_LISTROMFS)) && ctx->romfs_ctx.header.header_size == ROMFS_HEADER_SIZE) {
        /* Pre-load the file/data entry caches. */
        ctx->romfs_ctx.directories = calloc(1, ctx->romfs_ctx.header.dir_meta_table_size);
        if (ctx->romfs_ctx.directories == NULL) {
            fprintf(stderr, "Failed to allocate RomFS directory cache!\n");
            exit(EXIT_FAILURE);
        }

        /* Switch RomFS has actual entries at table offset + 4 for no good reason. */
        nca_section_fseek(ctx, ctx->romfs_ctx.romfs_offset + ctx->romfs_ctx.header.dir_meta_table_offset + 4);
        if (nca_section_fread(ctx, ctx->romfs_ctx.directories, ctx->romfs_ctx.header.dir_meta_table_size) != ctx->romfs_ctx.header.dir_meta_table_size) {
            fprintf(stderr, "Failed to read RomFS directory cache!\n");
            exit(EXIT_FAILURE);
        }

        ctx->romfs_ctx.files = calloc(1, ctx->romfs_ctx.header.file_meta_table_size);
        if (ctx->romfs_ctx.files == NULL) {
            fprintf(stderr, "Failed to allocate RomFS file cache!\n");
            exit(EXIT_FAILURE);
        }
        nca_section_fseek(ctx, ctx->romfs_ctx.romfs_offset + ctx->romfs_ctx.header.file_meta_table_offset);
        if (nca_section_fread(ctx, ctx->romfs_ctx.files, ctx->romfs_ctx.header.file_meta_table_size) != ctx->romfs_ctx.header.file_meta_table_size) {
            fprintf(stderr, "Failed to read RomFS file cache!\n");
            exit(EXIT_FAILURE);
        }
    }
}

void nca_process_bktr_section(nca_section_ctx_t *ctx) {
    bktr_superblock_t *sb = ctx->bktr_ctx.superblock;
    /* Validate magics. */
    if (sb->relocation_header.magic == MAGIC_BKTR && sb->subsection_header.magic == MAGIC_BKTR) {
        if (sb->relocation_header.offset + sb->relocation_header.size != sb->subsection_header.offset ||
            sb->subsection_header.offset + sb->subsection_header.size != ctx->size) {
            fprintf(stderr, "Invalid BKTR layout!\n");
            exit(EXIT_FAILURE);
        }
        /* Allocate space for an extra (fake) relocation entry, to simplify our logic. */
        void *relocs = calloc(1, sb->relocation_header.size + sizeof(bktr_relocation_entry_t));
        if (relocs == NULL) {
            fprintf(stderr, "Failed to allocate relocation header!\n");
            exit(EXIT_FAILURE);
        }
        /* Allocate space for an extra (fake) subsection entry, to simplify our logic. */
        void *subs = calloc(1, sb->subsection_header.size + sizeof(bktr_subsection_entry_t));
        if (subs == NULL) {
            fprintf(stderr, "Failed to allocate subsection header!\n");
            exit(EXIT_FAILURE);
        }
        nca_section_fseek(ctx, sb->relocation_header.offset);
        if (nca_section_fread(ctx, relocs, sb->relocation_header.size) != sb->relocation_header.size) {
            fprintf(stderr, "Failed to read relocation header!\n");
            exit(EXIT_FAILURE);
        }
        nca_section_fseek(ctx, sb->subsection_header.offset);
        if (nca_section_fread(ctx, subs, sb->subsection_header.size) != sb->subsection_header.size) {
            fprintf(stderr, "Failed to read subsection header!\n");
            exit(EXIT_FAILURE);
        }
        
        /* NOTE: Setting these variables changes the way fseek/fread work! */
        ctx->bktr_ctx.relocation_block = relocs;
        ctx->bktr_ctx.subsection_block = subs;
        
        /* This simplifies logic greatly... */
        ctx->bktr_ctx.relocation_block->entries[ctx->bktr_ctx.relocation_block->num_entries].virt_offset = ctx->bktr_ctx.relocation_block->patch_romfs_size;
        ctx->bktr_ctx.subsection_block->entries[ctx->bktr_ctx.subsection_block->num_entries].offset = sb->relocation_header.offset;
        ctx->bktr_ctx.subsection_block->entries[ctx->bktr_ctx.subsection_block->num_entries].ctr_val = ctx->header->section_ctr_low;
        
        
        /* Now parse out the romfs stuff. */
        for (unsigned int i = 0; i < IVFC_MAX_LEVEL; i++) {
            /* Load in the current level's header data. */
            ivfc_level_ctx_t *cur_level = &ctx->bktr_ctx.ivfc_levels[i];
            cur_level->data_offset = sb->ivfc_header.level_headers[i].logical_offset;
            cur_level->data_size = sb->ivfc_header.level_headers[i].hash_data_size;
            cur_level->hash_block_size = 1 << sb->ivfc_header.level_headers[i].block_size;

            if (i != 0) {
                /* Hash table is previous level's data. */
                cur_level->hash_offset = ctx->bktr_ctx.ivfc_levels[i-1].data_offset;
            } else if (ctx->tool_ctx->base_file != NULL) {
                /* Hash table is the superblock hash. Always check the superblock hash. */
                ctx->superblock_hash_validity = nca_section_check_external_hash_table(ctx, sb->ivfc_header.master_hash, cur_level->data_offset, cur_level->data_size, cur_level->hash_block_size, 1);
                cur_level->hash_validity = ctx->superblock_hash_validity;
            }
            if (ctx->tool_ctx->action & ACTION_VERIFY && i != 0) {
                /* Actually check the table. */
                printf("    Verifying IVFC Level %"PRId32"...\n", i);
                cur_level->hash_validity = nca_section_check_hash_table(ctx, cur_level->hash_offset, cur_level->data_offset, cur_level->data_size, cur_level->hash_block_size, 1);
            }
        }

        ctx->bktr_ctx.romfs_offset = ctx->bktr_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1].data_offset;
        if (ctx->tool_ctx->base_file != NULL) {
            nca_section_fseek(ctx, ctx->bktr_ctx.romfs_offset);
            if (nca_section_fread(ctx, &ctx->bktr_ctx.header, sizeof(romfs_hdr_t)) != sizeof(romfs_hdr_t)) {
                fprintf(stderr, "Failed to read BKTR Virtual RomFS header!\n");
            }

            if ((ctx->tool_ctx->action & (ACTION_EXTRACT | ACTION_LISTROMFS)) && ctx->bktr_ctx.header.header_size == ROMFS_HEADER_SIZE) {
                /* Pre-load the file/data entry caches. */
                ctx->bktr_ctx.directories = calloc(1, ctx->bktr_ctx.header.dir_meta_table_size);
                if (ctx->bktr_ctx.directories == NULL) {
                    fprintf(stderr, "Failed to allocate RomFS directory cache!\n");
                    exit(EXIT_FAILURE);
                }

                /* Switch RomFS has actual entries at table offset + 4 for no good reason. */
                nca_section_fseek(ctx, ctx->bktr_ctx.romfs_offset + ctx->bktr_ctx.header.dir_meta_table_offset + 4);
                if (nca_section_fread(ctx, ctx->bktr_ctx.directories, ctx->bktr_ctx.header.dir_meta_table_size) != ctx->bktr_ctx.header.dir_meta_table_size) {
                    fprintf(stderr, "Failed to read RomFS directory cache!\n");
                    exit(EXIT_FAILURE);
                }
                ctx->bktr_ctx.files = calloc(1, ctx->bktr_ctx.header.file_meta_table_size);
                if (ctx->bktr_ctx.files == NULL) {
                    fprintf(stderr, "Failed to allocate RomFS file cache!\n");
                    exit(EXIT_FAILURE);
                }
                nca_section_fseek(ctx, ctx->bktr_ctx.romfs_offset + ctx->bktr_ctx.header.file_meta_table_offset);
                if (nca_section_fread(ctx, ctx->bktr_ctx.files, ctx->bktr_ctx.header.file_meta_table_size) != ctx->bktr_ctx.header.file_meta_table_size) {
                    fprintf(stderr, "Failed to read RomFS file cache!\n");
                    exit(EXIT_FAILURE);
                }
            }
        }
    }
}

void nca_print_pfs0_section(nca_section_ctx_t *ctx) {
    if (ctx->tool_ctx->action & ACTION_VERIFY) {
        if (ctx->superblock_hash_validity == VALIDITY_VALID) {
            memdump(stdout, "        Superblock Hash (GOOD):     ", &ctx->pfs0_ctx.superblock->master_hash, 0x20);
        } else {
            memdump(stdout, "        Superblock Hash (FAIL):     ", &ctx->pfs0_ctx.superblock->master_hash, 0x20);
        }
        printf("        Hash Table (%s):\n", GET_VALIDITY_STR(ctx->pfs0_ctx.hash_table_validity));
    } else {
        memdump(stdout, "        Superblock Hash:            ", &ctx->pfs0_ctx.superblock->master_hash, 0x20);
        printf("        Hash Table:\n");
    }
    printf("            Offset:                 %012"PRIx64"\n", ctx->pfs0_ctx.superblock->hash_table_offset);
    printf("            Size:                   %012"PRIx64"\n", ctx->pfs0_ctx.superblock->hash_table_size);
    printf("            Block Size:             0x%"PRIx32"\n", ctx->pfs0_ctx.superblock->block_size);
    printf("        PFS0 Offset:                %012"PRIx64"\n", ctx->pfs0_ctx.superblock->pfs0_offset);
    printf("        PFS0 Size:                  %012"PRIx64"\n", ctx->pfs0_ctx.superblock->pfs0_size);
}

void nca_print_ivfc_section(nca_section_ctx_t *ctx) {
    if (ctx->tool_ctx->action & ACTION_VERIFY) {
        if (ctx->superblock_hash_validity == VALIDITY_VALID) {
            memdump(stdout, "        Superblock Hash (GOOD):     ",  &ctx->romfs_ctx.superblock->ivfc_header.master_hash, 0x20);
        } else {
            memdump(stdout, "        Superblock Hash (FAIL):     ",  &ctx->romfs_ctx.superblock->ivfc_header.master_hash, 0x20);
        }
    } else {
            memdump(stdout, "        Superblock Hash:            ", &ctx->romfs_ctx.superblock->ivfc_header.master_hash, 0x20);
    } 
    print_magic("        Magic:                      ", ctx->romfs_ctx.superblock->ivfc_header.magic);
    printf("        ID:                         %08"PRIx32"\n", ctx->romfs_ctx.superblock->ivfc_header.id);
    for (unsigned int i = 0; i < IVFC_MAX_LEVEL; i++) {
        if (ctx->tool_ctx->action & ACTION_VERIFY) {
            printf("        Level %"PRId32" (%s):\n", i, GET_VALIDITY_STR(ctx->romfs_ctx.ivfc_levels[i].hash_validity));
        } else {
            printf("        Level %"PRId32":\n", i);
        }
        printf("            Data Offset:            0x%012"PRIx64"\n", ctx->romfs_ctx.ivfc_levels[i].data_offset);
        printf("            Data Size:              0x%012"PRIx64"\n", ctx->romfs_ctx.ivfc_levels[i].data_size);
        if (i != 0) printf("            Hash Offset:            0x%012"PRIx64"\n", ctx->romfs_ctx.ivfc_levels[i].hash_offset);
        printf("            Hash Block Size:        0x%08"PRIx32"\n", ctx->romfs_ctx.ivfc_levels[i].hash_block_size);
    }
}

void nca_print_bktr_section(nca_section_ctx_t *ctx) {
    if (ctx->bktr_ctx.subsection_block == NULL) {
        printf("        BKTR section seems to be corrupted.\n");
        return;
    }
    int did_verify = (ctx->tool_ctx->action & ACTION_VERIFY) && (ctx->tool_ctx->base_file != NULL);
    if (did_verify ) {
        if (ctx->superblock_hash_validity == VALIDITY_VALID) {
            memdump(stdout, "        Superblock Hash (GOOD):     ",  &ctx->bktr_ctx.superblock->ivfc_header.master_hash, 0x20);
        } else {
            memdump(stdout, "        Superblock Hash (FAIL):     ",  &ctx->bktr_ctx.superblock->ivfc_header.master_hash, 0x20);
        }
    } else {
            memdump(stdout, "        Superblock Hash:            ", &ctx->bktr_ctx.superblock->ivfc_header.master_hash, 0x20);
    } 
    print_magic("        Magic:                      ", ctx->bktr_ctx.superblock->ivfc_header.magic);
    printf("        ID:                         %08"PRIx32"\n", ctx->bktr_ctx.superblock->ivfc_header.id);
    for (unsigned int i = 0; i < IVFC_MAX_LEVEL; i++) {
        if (did_verify) {
            printf("        Level %"PRId32" (%s):\n", i, GET_VALIDITY_STR(ctx->bktr_ctx.ivfc_levels[i].hash_validity));
        } else {
            printf("        Level %"PRId32":\n", i);
        }
        printf("            Data Offset:            0x%012"PRIx64"\n", ctx->bktr_ctx.ivfc_levels[i].data_offset);
        printf("            Data Size:              0x%012"PRIx64"\n", ctx->bktr_ctx.ivfc_levels[i].data_size);
        if (i != 0) printf("            Hash Offset:            0x%012"PRIx64"\n", ctx->bktr_ctx.ivfc_levels[i].hash_offset);
        printf("            Hash Block Size:        0x%08"PRIx32"\n", ctx->bktr_ctx.ivfc_levels[i].hash_block_size);
    }
}

void nca_save_section_file(nca_section_ctx_t *ctx, uint64_t ofs, uint64_t total_size, filepath_t *filepath) {    
    FILE *f_out = os_fopen(filepath->os_path, OS_MODE_WRITE);

    if (f_out == NULL) {
        fprintf(stderr, "Failed to open %s!\n", filepath->char_path);
        return;
    }

    uint64_t read_size = 0x400000; /* 4 MB buffer. */
    unsigned char *buf = malloc(read_size);
    if (buf == NULL) {
        fprintf(stderr, "Failed to allocate file-save buffer!\n");
        exit(EXIT_FAILURE);
    }
    memset(buf, 0xCC, read_size); /* Debug in case I fuck this up somehow... */
    uint64_t end_ofs = ofs + total_size;
    nca_section_fseek(ctx, ofs);
    while (ofs < end_ofs) {       
        if (ofs + read_size >= end_ofs) read_size = end_ofs - ofs;
        if (nca_section_fread(ctx, buf, read_size) != read_size) {
            fprintf(stderr, "Failed to read file!\n");
            exit(EXIT_FAILURE);
        }
        if (fwrite(buf, 1, read_size, f_out) != read_size) {
            fprintf(stderr, "Failed to write file!\n");
            exit(EXIT_FAILURE);
        }
        ofs += read_size;
    }

    fclose(f_out);

    free(buf);
}

void nca_save_section(nca_section_ctx_t *ctx) {
    /* Save raw section file... */
    uint64_t offset = 0;
    uint64_t size = ctx->size;
    if (!(ctx->tool_ctx->action & ACTION_RAW)) {
        switch (ctx->type) {
            case PFS0:
                offset = ctx->pfs0_ctx.superblock->pfs0_offset;
                size = ctx->pfs0_ctx.superblock->pfs0_size;
                break;
            case ROMFS:
                offset = ctx->romfs_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1].data_offset;
                size = ctx->romfs_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1].data_size;
                break;
            case BKTR:
                offset = ctx->bktr_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1].data_offset;
                size = ctx->bktr_ctx.ivfc_levels[IVFC_MAX_LEVEL - 1].data_size;
                break;
            case INVALID:
                break;
        }
    } else if (ctx->type == BKTR && ctx->bktr_ctx.subsection_block != NULL && ctx->tool_ctx->base_file != NULL) {
        size = ctx->bktr_ctx.relocation_block->patch_romfs_size;
    }
    
    /* Extract to file. */
    filepath_t *secpath = &ctx->tool_ctx->settings.section_paths[ctx->section_num];

    /* Handle overrides. */
    if (ctx->type == PFS0 && ctx->pfs0_ctx.is_exefs && ctx->tool_ctx->settings.exefs_path.enabled && ctx->tool_ctx->settings.exefs_path.path.valid == VALIDITY_VALID) {
        secpath = &ctx->tool_ctx->settings.exefs_path.path;
    } else if (ctx->type == ROMFS && ctx->tool_ctx->settings.romfs_path.enabled && ctx->tool_ctx->settings.romfs_path.path.valid == VALIDITY_VALID) {
        secpath = &ctx->tool_ctx->settings.romfs_path.path;
    }
    if (secpath != NULL && secpath->valid == VALIDITY_VALID) {
        printf("Saving Section %"PRId32" to %s...\n", ctx->section_num, secpath->char_path);
        nca_save_section_file(ctx, offset, size, secpath);
    }

    switch (ctx->type) {
        case PFS0:
            nca_save_pfs0_section(ctx);
            break;
        case ROMFS:
            nca_save_ivfc_section(ctx);
            break;
        case BKTR:
            if (ctx->tool_ctx->base_file == NULL) {
                fprintf(stderr, "Note: cannot save BKTR section without base romfs.\n");
                break;
            }
            nca_save_bktr_section(ctx);
            break;
        case INVALID:
            break;
    }
}

void nca_save_pfs0_section(nca_section_ctx_t *ctx) {
    if (ctx->superblock_hash_validity == VALIDITY_VALID && ctx->pfs0_ctx.header->magic == MAGIC_PFS0) {
        /* Extract to directory. */
        filepath_t *dirpath = NULL;
        if (ctx->pfs0_ctx.is_exefs && ctx->tool_ctx->settings.exefs_dir_path.enabled) {
            dirpath = &ctx->tool_ctx->settings.exefs_dir_path.path;
        }
        if (dirpath == NULL || dirpath->valid != VALIDITY_VALID) {
            dirpath = &ctx->tool_ctx->settings.section_dir_paths[ctx->section_num];
        }
        if (dirpath != NULL && dirpath->valid == VALIDITY_VALID) {
            os_makedir(dirpath->os_path);
            for (uint32_t i = 0; i < ctx->pfs0_ctx.header->num_files; i++) {
                nca_save_pfs0_file(ctx, i, dirpath);
            }
        }
    } else {
        fprintf(stderr, "Error: section %"PRId32" is corrupted!\n", ctx->section_num);
        return;
    }
}

/* RomFS functions... */
void nca_visit_romfs_file(nca_section_ctx_t *ctx, uint32_t file_offset, filepath_t *dir_path) {
    romfs_fentry_t *entry;
    if (ctx->type == ROMFS) {
        entry = romfs_get_fentry(ctx->romfs_ctx.files, file_offset);
    } else {
        entry = romfs_get_fentry(ctx->bktr_ctx.files, file_offset);
    }
    filepath_t *cur_path = calloc(1, sizeof(filepath_t));
    if (cur_path == NULL) {
        fprintf(stderr, "Failed to allocate filepath!\n");
        exit(EXIT_FAILURE);
    }

    filepath_copy(cur_path, dir_path);
    if (entry->name_size) {
        filepath_append_n(cur_path, entry->name_size, "%s", entry->name);
    }

    /* If we're extracting... */
    if ((ctx->tool_ctx->action & ACTION_LISTROMFS) == 0) {
        printf("Saving %s...\n", cur_path->char_path);
        uint64_t phys_offset;
        if (ctx->type == ROMFS) {
            phys_offset = ctx->romfs_ctx.romfs_offset + ctx->romfs_ctx.header.data_offset + entry->offset;
        } else {
            phys_offset = ctx->bktr_ctx.romfs_offset + ctx->bktr_ctx.header.data_offset + entry->offset;
        }
        nca_save_section_file(ctx, phys_offset, entry->size, cur_path);
    } else {
        printf("rom:%s\n", cur_path->char_path);
    }

    free(cur_path);

    if (entry->sibling != ROMFS_ENTRY_EMPTY) {
        nca_visit_romfs_file(ctx, entry->sibling, dir_path);
    }
}

void nca_visit_romfs_dir(nca_section_ctx_t *ctx, uint32_t dir_offset, filepath_t *parent_path) {
    romfs_direntry_t *entry;
    if (ctx->type == ROMFS) {
        entry = romfs_get_direntry(ctx->romfs_ctx.directories, dir_offset);
    } else {
        entry = romfs_get_direntry(ctx->bktr_ctx.directories, dir_offset);
    } 
    filepath_t *cur_path = calloc(1, sizeof(filepath_t));
    if (cur_path == NULL) {
        fprintf(stderr, "Failed to allocate filepath!\n");
        exit(EXIT_FAILURE);
    }

    filepath_copy(cur_path, parent_path);
    if (entry->name_size) {
        filepath_append_n(cur_path, entry->name_size, "%s", entry->name);
    }

    /* If we're actually extracting the romfs, make directory. */
    if ((ctx->tool_ctx->action & ACTION_LISTROMFS) == 0) {
        os_makedir(cur_path->os_path);
    }

    if (entry->file != ROMFS_ENTRY_EMPTY) {
        nca_visit_romfs_file(ctx, entry->file, cur_path);
    }
    if (entry->child != ROMFS_ENTRY_EMPTY) {
        nca_visit_romfs_dir(ctx, entry->child, cur_path);
    }
    if (entry->sibling != ROMFS_ENTRY_EMPTY) {
        nca_visit_romfs_dir(ctx, entry->sibling, parent_path);
    }

    free(cur_path);
}


void nca_save_ivfc_section(nca_section_ctx_t *ctx) {
    if (ctx->superblock_hash_validity == VALIDITY_VALID) {
        if (ctx->romfs_ctx.header.header_size == ROMFS_HEADER_SIZE) {
            if (ctx->tool_ctx->action & ACTION_LISTROMFS) {
                filepath_t fakepath;
                filepath_init(&fakepath);
                filepath_set(&fakepath, "");

                nca_visit_romfs_dir(ctx, 0, &fakepath);
            } else {
                filepath_t *dirpath = NULL;
                if (ctx->tool_ctx->settings.romfs_dir_path.enabled) {
                    dirpath = &ctx->tool_ctx->settings.romfs_dir_path.path;
                }
                if (dirpath == NULL || dirpath->valid != VALIDITY_VALID) {
                    dirpath = &ctx->tool_ctx->settings.section_dir_paths[ctx->section_num];
                }
                if (dirpath != NULL && dirpath->valid == VALIDITY_VALID) {
                    os_makedir(dirpath->os_path);
                    nca_visit_romfs_dir(ctx, 0, dirpath);
                }
            }

            return;
        }
    }

    fprintf(stderr, "Error: section %"PRId32" is corrupted!\n", ctx->section_num);
}

void nca_save_bktr_section(nca_section_ctx_t *ctx) {
    if (ctx->superblock_hash_validity == VALIDITY_VALID) {
        if (ctx->bktr_ctx.header.header_size == ROMFS_HEADER_SIZE) {
            if (ctx->tool_ctx->action & ACTION_LISTROMFS) {
                filepath_t fakepath;
                filepath_init(&fakepath);
                filepath_set(&fakepath, "");

                nca_visit_romfs_dir(ctx, 0, &fakepath);
            } else {
                filepath_t *dirpath = NULL;
                if (ctx->tool_ctx->settings.romfs_dir_path.enabled) {
                    dirpath = &ctx->tool_ctx->settings.romfs_dir_path.path;
                }
                if (dirpath == NULL || dirpath->valid != VALIDITY_VALID) {
                    dirpath = &ctx->tool_ctx->settings.section_dir_paths[ctx->section_num];
                }
                if (dirpath != NULL && dirpath->valid == VALIDITY_VALID) {
                    os_makedir(dirpath->os_path);
                    nca_visit_romfs_dir(ctx, 0, dirpath);
                }
            }

            return;
        }
    }

    fprintf(stderr, "Error: section %"PRId32" is corrupted!\n", ctx->section_num);
}
