/* copyright (C) Comcast 2025 */
/* author: ab */
/* 1/29/2025 */

#include <filesystem>
#include <cstring>
#include "pugixml.hpp"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SEGBUILD
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#else
#include "inband_main.h"
#endif

#define CURMPD (const char*)"cur.mpd"
#define SCANBUF 256


typedef struct context_s {
    uint64_t  tfdt;
    uint32_t  timescale;
    size_t    mpdsz;
    uint8_t   version;
    char      pubtime[32];
    const char*  mpd_name;
    const char*  audio_seg_name;
    uint8_t*     audio_seg_contents;
    size_t       audio_seg_sz;
} context_t;


void get_timescale(ngx_http_request_t* r, context_t* ctx) {
    char*  pt = NULL;
    size_t pt_sz;
    ctx->timescale = 0;

    //get the 1) publish time 2) audio timescale 3) CURMPD size in bytes
    pugi::xml_document doc;
    if (!doc.load_file(CURMPD))
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "_INBAND_ could not open mpd file \"%s\" for reading\n", CURMPD);
        return;
    }

    pugi::xml_node mpdxml = doc.child("MPD");
    pt = (char*)mpdxml.attribute("publishTime").value();
    pt_sz = strlen(pt);
    if (pt_sz == 0)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "_INBAND_ publishTime string not found\n");
        return;
    }

    memcpy(ctx->pubtime, pt, pt_sz);
    ctx->pubtime[pt_sz] = '\0';

    pugi::xml_node period = mpdxml.child("Period");
    for (pugi::xml_node adaptset = period.first_child(); adaptset; adaptset = adaptset.next_sibling())
        if (strncmp(adaptset.attribute("contentType").value(), "audio", 5) == 0)
        {
            ctx->timescale = strtoul(adaptset.child("SegmentTemplate").attribute("timescale").value(), NULL, 10);
            break;
        }

    if (ctx->timescale == 0)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "_INBAND_ could not find audio timescale\n");
        return;
    }

    ctx->mpdsz = std::filesystem::file_size(CURMPD);
}

void get_tfdt(ngx_http_request_t* r, context_t* ctx) {
    FILE*     fp;
    long      loc = -1;
    size_t    num;

    fp = fopen(ctx->audio_seg_name, "rb");
    if (fp == NULL)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "_INBAND_ could not open audio seg file \"%s\"\n", ctx->audio_seg_name);
        return;
    }

    ctx->audio_seg_contents = (uint8_t*)calloc(ctx->audio_seg_sz, 1);
    if (ctx->audio_seg_contents == NULL)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "_INBAND_ get_tfdt, calloc returned NULL\n");
        return;
    }

    num = fread(ctx->audio_seg_contents, 1, ctx->audio_seg_sz, fp);
    if (num < ctx->audio_seg_sz)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "_INBAND_ num less than file sz num: %lu sz: %lu\n", num, ctx->audio_seg_sz);
        return;
    }

    fclose(fp);

    for (int i = 0; i < SCANBUF; i++)
    {
        if (ctx->audio_seg_contents[i] == 't')
            if (memcmp("fdt", &(ctx->audio_seg_contents[i+1]), 3) == 0)
            {
                loc = i;
                break;
            }
    }

    if (loc < 0)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "_INBAND_ did not find tfdt, exiting\n");
        exit(1);
    }
    else
    {   //extract 'base media decode time'
        uint8_t   version_buf[4];
        uint8_t   tfdt_buf[8];

        loc += 4;
        memcpy(version_buf, &ctx->audio_seg_contents[loc], 4);
        loc += 4;
        memcpy(tfdt_buf, &ctx->audio_seg_contents[loc], 8);

        ctx->version = ((uint32_t)(version_buf[3]) << 24) |
            ((uint32_t)(version_buf[2]) << 16) |
            ((uint32_t)(version_buf[1]) <<  8) |
            ((uint32_t)(version_buf[0])      );

        ctx->tfdt = ((uint64_t)(tfdt_buf[0]) << 56) |
            ((uint64_t)(tfdt_buf[1]) << 48) |
            ((uint64_t)(tfdt_buf[2]) << 40) |
            ((uint64_t)(tfdt_buf[3]) << 32) |
            ((uint64_t)(tfdt_buf[4]) << 24) |
            ((uint64_t)(tfdt_buf[5]) << 16) |
            ((uint64_t)(tfdt_buf[6]) <<  8) |
            ((uint64_t)(tfdt_buf[7]) );
    }
}

void int2buf32(uint32_t val, uint8_t* valbuf) {
    valbuf[0] = (uint32_t)val >> 24;
    valbuf[1] = (uint32_t)val >> 16;
    valbuf[2] = (uint32_t)val >> 8;
    valbuf[3] = (uint32_t)val;
}

void int2buf64(uint64_t val, uint8_t* valbuf) {
    valbuf[0] = (uint64_t)val >> 56;
    valbuf[1] = (uint64_t)val >> 48;
    valbuf[2] = (uint64_t)val >> 40;
    valbuf[3] = (uint64_t)val >> 32;
    valbuf[4] = (uint64_t)val >> 24;
    valbuf[5] = (uint64_t)val >> 16;
    valbuf[6] = (uint64_t)val >> 8;
    valbuf[7] = (uint64_t)val;
}

void write_styp(FILE* fp) {
    uint8_t  styp_sz_buf[4];
    uint32_t zero = 0x00;
    uint8_t  styp[4] = {'s', 't', 'y', 'p'};
    uint8_t  iso9[4] = {'i', 's', 'o', '9'};
    uint8_t  dash[4] = {'d', 'a', 's', 'h'};

    uint8_t  seg_buf[20];
    uint32_t styp_sz = 20; //8 for sz & header tag + 4*3

    int2buf32(styp_sz, styp_sz_buf);

    memcpy(seg_buf, styp_sz_buf, 4);
    memcpy(&seg_buf[4], styp, 4);
    memcpy(&seg_buf[8], iso9, 4);
    memcpy(&seg_buf[12], &zero, 4);
    memcpy(&seg_buf[16], dash, 4);

    fwrite(seg_buf, 1, 20, fp);
    fflush(fp);
}

void write_emsg(ngx_http_request_t* r, FILE* fp, context_t* ctx) {
    uint32_t emsg_sz;
    uint8_t  emsg_sz_buf[4];
    uint8_t  emsg[4] = {'e', 'm', 's', 'g'};
    uint32_t pubtime_sz;
    uint32_t siu_sz;
    uint8_t  version = 0x01;
    uint8_t  flags[3] = {0x00};
    uint8_t  timescale_buf[4];
    uint8_t  tfdt_buf[8];
    uint8_t  event_dur[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t id = 0xDEADBEEF;
    uint8_t  value[2] = {'3', '\0'};
    size_t   num;

    const char* scheme_id_uri = (const char*)"urn:mpeg:dash:event:2012";

    pubtime_sz = strlen(ctx->pubtime);
    siu_sz = strlen(scheme_id_uri);
    emsg_sz = 8 +             //emsg_sz & 'emsg'
              24 +            //len of constant fields
              siu_sz + 1 +    //+1 for '\0'
              2 +             //len of "value" field
              pubtime_sz + 1 +//+1 for '\0'
              ctx->mpdsz;

    uint8_t* seg_buf = (uint8_t*)calloc(emsg_sz, 1);
    if (seg_buf == NULL)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "_INBAND_ write_emsg, calloc returned NULL\n");
        return;
    }

    int2buf32(emsg_sz, emsg_sz_buf);
    memcpy(seg_buf, emsg_sz_buf, 4);
    memcpy(&seg_buf[4], emsg, 4);
    memcpy(&seg_buf[8], &version, 1);
    memcpy(&seg_buf[9], flags, 3);

    int2buf32(ctx->timescale, timescale_buf);
    memcpy(&seg_buf[12], timescale_buf, 4);

    int2buf64(ctx->tfdt, tfdt_buf);
    memcpy(&seg_buf[16], tfdt_buf, 8);
    memcpy(&seg_buf[24], event_dur, 4);
    memcpy(&seg_buf[28], &id, 4);
    memcpy(&seg_buf[32], scheme_id_uri, 25); //25 = strlen(scheme_id_uri)+1
    memcpy(&seg_buf[57], &value, 2);
    memcpy(&seg_buf[59], ctx->pubtime, pubtime_sz+1);

    //read in the cur mpd to seg_buf[59+pubtime_sz+1]
    FILE *fmpd = fopen(CURMPD, "rb");
    num = fread(&seg_buf[59+pubtime_sz+1], 1, ctx->mpdsz, fmpd);
    if (num < ctx->mpdsz)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "_INBAND_ write_emsg num less than mpdsz. num: %lu mpdsz: %ld\n",
                      num, ctx->mpdsz);
        return;
    }
    fclose(fmpd);

    //write to file
    fwrite(seg_buf, emsg_sz, 1, fp);
    fflush(fp);
    free(seg_buf);
    seg_buf = NULL;
}

void concat_audio_seg(ngx_http_request_t* r, FILE* fp, context_t* ctx) {
    if (ctx->audio_seg_contents == NULL)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "_INBAND_ concat_audio_seg: no seg contents, exiting \n");
        exit(1);
    }
    fwrite(ctx->audio_seg_contents, ctx->audio_seg_sz, 1, fp);
    fflush(fp);
    free(ctx->audio_seg_contents);
    ctx->audio_seg_contents = NULL;
}


void inband_process_audio(ngx_http_request_t* r, const char *path_str, size_t size) {
    context_t ctx;

    /* get the audio seg temp file name and size */
    ctx.audio_seg_name = path_str;
    ctx.audio_seg_sz = size;

    get_timescale(r, &ctx);
    get_tfdt(r, &ctx);

    FILE* fp;
    fp = fopen(ctx.audio_seg_name, "wb");
    if (fp == NULL)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "_INBAND_ could not open audio seg file \"%s\" for writing\n", ctx.audio_seg_name);
        return;
    }
    write_styp(fp);
    write_emsg(r, fp, &ctx);
    concat_audio_seg(r, fp, &ctx);
    fclose(fp);
}

void inband_process_mpd(ngx_http_request_t* r, const char *path_str) {

    const char* incoming = (const char*)r->request_body->temp_file->file.name.data;
    pugi::xml_document doc;

    pugi::xml_parse_result result = doc.load_file(path_str);
    if (result.status != pugi::status_ok)
    {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "_INBAND_ could not open incoming mpd file \"%s\": %s\n",
                      path_str, result.description());
        return;
    }

    pugi::xml_node mpd = doc.child("MPD");

    //add minimumUpdatePeriod="PT0S"
    mpd.remove_attribute("minimumUpdatePeriod");
    pugi::xml_attribute mup = mpd.append_attribute("minimumUpdatePeriod");
    mup.set_value("PT0S");

    pugi::xpath_query query_scan_type("/MPD/Period/AdaptationSet");
    pugi::xpath_node_set scan_items = query_scan_type.evaluate_node_set(doc);
    for (pugi::xpath_node_set::const_iterator it = scan_items.begin(); it != scan_items.end(); ++it)
    {
        pugi::xpath_node node = *it;
        std::string val = node.node().attribute("contentType").value();
        if (val == "audio")
        {
            //add InbandEventStream
            node.node().remove_child("InbandEventStream");
            pugi::xml_node ies = node.node().append_child("InbandEventStream");

            pugi::xml_attribute siu = ies.append_attribute("schemeIdUri");
            siu.set_value("urn:mpeg:dash:event:2012");

            pugi::xml_attribute val = ies.append_attribute("value");
            val.set_value("3");
            break;
        }
    }

    //cache this mpd as our CURMPD
    doc.save_file(CURMPD);
    //save for serving to requests
    doc.save_file(path_str);
}

void inband_process(ngx_http_request_t* r, u_char* path_str) {
    char* point = NULL;

    if ( (point = strrchr((char*)path_str,'.')) != NULL )
    {
        if (strcmp(point,".mp4a") == 0)
            inband_process_audio(r, (const char *)path_str, r->request_body->temp_file->file.offset);
        else if (strcmp(point,".mpd") == 0)
            inband_process_mpd(r, (const char *)path_str);
    }
}

#ifdef __cplusplus
}
#endif

