// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "iwl_firmware.h"
int main(int argc, char **argv)
{
    FILE *file;
    uint8_t *data;
    IWL_FW_PLAN *plan;
    long size;
    int result;
    if (argc != 2) {
        fprintf(stderr, "Usage: firmware-inspect <iwx-so-a0-gf-a0-77>\n");
        return 2;
    }
    file = fopen(argv[1], "rb");
    if (!file) { perror("firmware"); return 2; }
    if (fseek(file,0,SEEK_END) || (size=ftell(file)) < 0 ||
        (unsigned long)size > IWL_FW_MAX_BYTES || fseek(file,0,SEEK_SET)) {
        fclose(file); return 2;
    }
    data=(uint8_t *)malloc((size_t)size+1);
    plan=(IWL_FW_PLAN *)malloc(sizeof(*plan));
    if (!data || !plan) { free(data);free(plan);fclose(file);return 2; }
    if (fread(data,1,(size_t)size,file)!=(size_t)size) {
        free(data);free(plan);fclose(file);return 2;
    }
    fclose(file);
    result=iwl_fw_parse(data,(size_t)size,plan);
    if (result) {
        fprintf(stderr,"%s\n",iwl_fw_error(result));
    } else {
        printf("{\"version\":\"%u.%08x.%u\",\"lmac_sections\":%u,"
            "\"umac_sections\":%u,\"paging_sections\":%u,\"paging_bytes\":%u,"
            "\"image_loader_bytes\":%u,\"section_bytes\":%u,"
            "\"records\":%u,\"loaded_on_hardware\":false}\n",
            plan->version_major,plan->version_minor,plan->version_api,
            plan->group_count[0],plan->group_count[1],plan->group_count[2],
            plan->paging_bytes,plan->iml_length,plan->total_section_bytes,
            plan->record_count);
    }
    free(plan);free(data);return result ? 1:0;
}
