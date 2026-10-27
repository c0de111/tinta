#include "web_firmware.h"
#include "config.h"
#include "use_case.h"
#define LOG_MODULE LOG_MOD_WEBSERVER
#include "debug.h"
#include "flash.h"
#include "hardware/flash.h"
#include "ota.h"
#include "webserver.h"
#include "webserver_scaffold.h"
#include "webserver_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *fw_use_case_name_from_id(uint8_t use_case_id) {
    switch (use_case_id) {
    case USE_CASE_ID_SEATSURFING:
        return "SeatSurfing";
    case USE_CASE_ID_HISTORIAN:
        return "Historian";
    case USE_CASE_ID_HOMEMATIC:
        return "Homematic";
    case USE_CASE_ID_WEATHERMAP:
        return "Weathermap";
    case USE_CASE_ID_NEXTCLOUD:
        return "Nextcloud";
    case USE_CASE_ID_NEW_USECASE:
        return "NewUseCase";
    default:
        return NULL;
    }
}

static bool fw_use_case_meta_valid(const firmware_header_t *header) {
    if (!header || header->meta_version != 1) {
        return false;
    }

    const char *expected_name = fw_use_case_name_from_id(header->use_case_id);
    if (!expected_name) {
        return false;
    }

    size_t name_len = 0;
    while (name_len < sizeof(header->use_case_name) && header->use_case_name[name_len] != '\0') {
        name_len++;
    }
    if (name_len == 0 || name_len == sizeof(header->use_case_name)) {
        return false;
    }

    return strcmp(header->use_case_name, expected_name) == 0;
}

void format_slot_use_case(uint8_t slot, char *out, size_t out_len, bool with_id) {
    if (!out || out_len == 0) {
        return;
    }

    uint32_t offset = (slot == 0)   ? FIRMWARE_SLOT0_FLASH_OFFSET
                      : (slot == 1) ? FIRMWARE_SLOT1_FLASH_OFFSET
                                    : 0;

    if (offset == 0) {
        snprintf(out, out_len, "n/a");
        return;
    }

    const firmware_header_t *header = (const firmware_header_t *)FLASH_PTR(offset);
    if (memcmp(header->magic, FIRMWARE_MAGIC, FIRMWARE_MAGIC_LEN) != 0 || header->valid_flag != 1) {
        snprintf(out, out_len, "empty");
        return;
    }

    if (fw_use_case_meta_valid(header)) {
        if (with_id) {
            snprintf(out, out_len, "%s (%u)", header->use_case_name, (unsigned)header->use_case_id);
        } else {
            snprintf(out, out_len, "%s", header->use_case_name);
        }
    } else if (header->meta_version == 0 && header->use_case_id == 0 &&
               header->use_case_name[0] == '\0') {
        snprintf(out, out_len, "legacy/unknown");
    } else {
        snprintf(out, out_len, "invalid metadata");
    }
}

void build_upload_logo_page(char *buf, size_t sz, const char *message) {
    char timeout_info[64];
    add_timeout_info(timeout_info, sizeof(timeout_info));

    int width, height, datalen;
    bool has_logo = get_flash_logo_info(&width, &height, &datalen);

    int n = scaffold_page_open(buf, sz, "Upload Logo", 0);

    if (message && *message) {
        const char *cls =
            (strncmp(message, "\xe2\x9d\x8c", 3) == 0 || strncmp(message, "\xe2\x9a\xa0", 3) == 0)
                ? "flash-error"
                : "flash-ok";
        n += snprintf(buf + n, sz - (size_t)n, "<p class=\"%s\">%s</p>", cls, message);
    }

    n += snprintf(
        buf + n, sz - (size_t)n,
        "<style>.hidden-input{position:absolute;left:-9999px}"
        ".fw-filemeta{font-size:.95em;color:#333;margin-top:.35em;text-align:center}</style>"
        "<div class=\"section\"><h2>Upload</h2>"
        "<input type='file' id='fileInput' class='hidden-input'>"
        "<div style='text-align:center'>"
        "<button type='button' onclick=\"document.getElementById('fileInput').click()\">"
        "Choose file</button></div>"
        "<div class='fw-filemeta' id='logoFN'>No file selected.</div>"
        "<div style='text-align:center'>"
        "<button type='button' onclick='upload()'>Upload</button></div>"
        "<progress id='progressBar' max='100' value='0' "
        "style='display:block;margin:.5em auto'></progress>"
        "<p id='status'></p>"
        "</div>"
        "<script>"
        "const MAX_SIZE=%d;"
        "function upload(){"
        "const file=document.getElementById('fileInput').files[0];"
        "if(!file)return;"
        "if(file.size>MAX_SIZE){"
        "document.getElementById('status').innerText='\xe2\x9d\x8c File too large ('+file.size+' "
        "bytes, max '+MAX_SIZE+' bytes allowed)';"
        "return;}"
        "const xhr=new XMLHttpRequest();"
        "xhr.open('POST','/upload_logo',true);"
        "xhr.setRequestHeader('Content-Type','application/octet-stream');"
        "xhr.upload.onprogress=function(e){"
        "if(e.lengthComputable){"
        "const pct=Math.round(e.loaded/e.total*100);"
        "document.getElementById('progressBar').value=pct;"
        "document.getElementById('status').innerText='Uploading: '+pct+'%%';}};"
        "xhr.onload=function(){"
        "if(xhr.status==200)document.getElementById('status').innerText='\xe2\x9c\x85 Upload OK';"
        "else document.getElementById('status').innerText='\xe2\x9d\x8c Upload failed';};"
        "xhr.onerror=function(){"
        "document.getElementById('status').innerText='\xe2\x9d\x8c Upload error';};"
        "xhr.send(file);}"
        "document.getElementById('fileInput').addEventListener('change',function(){"
        "var fn=document.getElementById('logoFN');"
        "var f=this.files&&this.files[0];"
        "if(fn)fn.textContent=f?f.name:'No file selected.';});"
        "</script>",
        LOGO_FLASH_SIZE);

    if (has_logo) {
        n += snprintf(buf + n, sz - (size_t)n,
                      "<div class=\"section\"><h2>Current Logo</h2>"
                      "<p>%d\xc3\x97%d px, %d bytes</p>"
                      "<form method=\"POST\" action=\"/delete_logo\">"
                      "<div style=\"text-align:center\">"
                      "<button type=\"submit\">Delete Logo</button>"
                      "</div></form></div>",
                      width, height, datalen);
    } else {
        n += snprintf(buf + n, sz - (size_t)n,
                      "<div class=\"section\"><p><i>No custom logo in flash.</i></p></div>");
    }

    dlog("upload_logo page length: %d\n", n);
    n += snprintf(buf + n, sz - (size_t)n, "<p class=\"small\">%s</p>", timeout_info);
    if ((size_t)n >= sz)
        n = (int)sz - 1;
    scaffold_page_close(buf + n, sz - (size_t)n, "/", NULL);
}

void build_firmware_update_page(char *buf, size_t sz, const char *message) {
    char timeout_info[64];
    add_timeout_info(timeout_info, sizeof(timeout_info));
    const int max_size = FIRMWARE_FLASH_SIZE;

    int n = scaffold_page_open(buf, sz, "Firmware Update", 0);

    n += snprintf(
        buf + n, sz - (size_t)n,
        "<style>"
        "#status{margin-top:1em;font-weight:bold;color:red}"
        ".spinner{width:48px;height:48px;border:6px solid #ccc;border-top-color:#4CAF50;"
        "border-radius:50%%;margin:1.5em auto;display:none;animation:spin 1s linear infinite}"
        ".spinner.active{display:block}"
        "@keyframes spin{to{transform:rotate(360deg)}}"
        ".demote{border:2px solid #c62828;background:#fff3e0;color:#333;padding:.9em;"
        "margin:1em auto;max-width:540px;text-align:left;border-radius:8px}"
        "button.demote-btn{background:#c62828;color:white;border:2px solid #c62828;"
        "padding:.6em .9em;border-radius:4px;cursor:pointer;display:block;margin:.8em auto 0}"
        "button.demote-btn:hover{background:#b71c1c}"
        ".hidden-input{position:absolute;left:-9999px}"
        ".fw-filemeta{font-size:.95em;color:#333;margin-top:.35em;text-align:center}"
        ".slot-info p{margin:.15em 0}"
        "</style>");

    if (message && *message) {
        n += snprintf(buf + n, sz - (size_t)n, "<p class=\"flash-ok\">%s</p>", message);
    }

    n +=
        snprintf(buf + n, sz - (size_t)n,
                 "<div class=\"section\">"
                 "<h2>Active Firmware</h2>"
                 "<p><span class=\"value\">%s</span></p>"
                 "<input type=\"file\" id=\"fileInput\" class=\"hidden-input\">"
                 "<div style=\"text-align:center\">"
                 "<button type=\"button\" onclick=\"document.getElementById('fileInput').click()\">"
                 "Choose firmware file</button>"
                 "</div>"
                 "<div class=\"fw-filemeta\" id=\"fwFileName\">No file selected.</div>"
                 "<div style=\"text-align:center\">"
                 "<button type=\"button\" onclick=\"upload()\">Upload</button>"
                 "</div>"
                 "<div id=\"spinner\" class=\"spinner\"></div>"
                 "<p id=\"status\"></p>"
                 "<div id=\"uploadResult\"></div>"
                 "</div>",
                 get_active_firmware_slot_info());

    firmware_slot_info_t info0 = {0}, info1 = {0};
    bool has0 = get_firmware_slot_info(0, &info0);
    bool has1 = get_firmware_slot_info(1, &info1);

    n += snprintf(buf + n, sz - (size_t)n, "<div class=\"section slot-info\"><h2>Slot Info</h2>");
    if (has0) {
        char uc[64] = "n/a";
        format_slot_use_case(0, uc, sizeof(uc), false);
        n += snprintf(buf + n, sz - (size_t)n, "<p>Slot 0: %s, %s, (%s), %u bytes</p>",
                      info0.git_version, uc, info0.build_date, info0.size);
    } else {
        n += snprintf(buf + n, sz - (size_t)n, "<p>Slot 0: <i>empty or invalid</i></p>");
    }
    if (has1) {
        char uc[64] = "n/a";
        format_slot_use_case(1, uc, sizeof(uc), false);
        n += snprintf(buf + n, sz - (size_t)n, "<p>Slot 1: %s, %s, (%s), %u bytes</p>",
                      info1.git_version, uc, info1.build_date, info1.size);
    } else {
        n += snprintf(buf + n, sz - (size_t)n, "<p>Slot 1: <i>empty or invalid</i></p>");
    }
    n += snprintf(buf + n, sz - (size_t)n, "</div>");

    n += snprintf(
        buf + n, sz - (size_t)n,
        "<div class=\"demote\">"
        "Demote the currently active slot metadata so the other valid slot is preferred on the "
        "next reboot. This way it is possible to boot from older builds."
        "<br><span style=\"color:#c62828\">Warning:</span> This rewrites firmware header metadata "
        "(version/build date) of the running slot. Use only if you understand the A/B update "
        "behavior."
        "<form method=\"POST\" action=\"/firmware_demote_active\" "
        "onsubmit=\"return confirm('Dangerous developer helper: demote ACTIVE slot metadata? "
        "Current firmware keeps running, but next reboot may switch to the other slot. "
        "Continue?');\" style=\"margin-top:.8em\">"
        "<button type=\"submit\" class=\"demote-btn\">Demote active slot priority</button>"
        "</form></div>");

    // The page picks the image for the slot that is not running: from a container (.tfw, both
    // slots behind "TINTAFW1", see build.sh) it cuts out that one, a single .bin must already
    // be the right one. The sign checks the image again after the transfer.
    const int target_slot = strncmp(get_active_firmware_slot_info(), "SLOT_0", 6) == 0 ? 1 : 0;
    n += snprintf(buf + n, sz - (size_t)n, "<script>const TARGET=%d,RUNNING='%s';", target_slot,
                  USE_CASE_NAME);
    n += snprintf(
        buf + n, sz - (size_t)n,
        "var sel=null;"
        "function hdr(b,o){var d=new DataView(b);"
        "var t=function(a,l){var s='';for(var i=0;i<l;i++){var c=d.getUint8(o+a+i);"
        "if(!c)break;s+=String.fromCharCode(c);}return s;};"
        "return{magic:t(0,13),ver:t(30,32),size:d.getUint32(o+62,true),"
        "slot:d.getUint8(o+66),uc:t(73,24)};}"
        "function pick(f){var m=document.getElementById('fwFileName');sel=null;"
        "m.textContent=f.name;"
        "f.arrayBuffer().then(function(b){var h=null,o=0;"
        "var head=String.fromCharCode.apply(null,new Uint8Array(b,0,Math.min(8,b.byteLength)));"
        "if(head==='TINTAFW1'){o=8;while(o+256<=b.byteLength){var x=hdr(b,o);"
        "if(x.magic!=='inki_firmware'||x.size<256)break;"
        "if(x.slot===TARGET){h=x;sel=f.slice(o,o+x.size);break;}o+=x.size;}}"
        "else if(b.byteLength>=256){var x=hdr(b,0);"
        "if(x.magic==='inki_firmware'&&x.slot===TARGET){h=x;sel=f;}}"
        "if(!sel){m.textContent=f.name+': no firmware for slot '+TARGET+' in this file.';"
        "return;}"
        "m.textContent=f.name+': writes '+h.ver+' ('+h.uc+') to slot '+TARGET+"
        "(h.uc!==RUNNING?' \u2014 other use case, its settings will be reset':'');});}");
    n += snprintf(buf + n, sz - (size_t)n,
                  "const MAX_SIZE=%d;"
                  "function upload(){"
                  "const file=sel;"
                  "if(!file)return;"
                  "if(file.size>MAX_SIZE){"
                  "document.getElementById('status').innerText="
                  "'File too large ('+file.size+' bytes, max '+MAX_SIZE+' bytes)';return;}"
                  "const xhr=new XMLHttpRequest();"
                  "xhr.open('POST','/firmware_update',true);"
                  "xhr.setRequestHeader('Content-Type','application/octet-stream');"
                  "xhr.setRequestHeader('X-Tinta-Slot',String(TARGET));"
                  "xhr.responseType='text';"
                  "document.getElementById('spinner').classList.add('active');"
                  "document.getElementById('status').innerText="
                  "'Uploading image\\u2026 please wait and leave the device untouched.';"
                  "document.getElementById('uploadResult').innerHTML='';"
                  "xhr.onerror=function(){"
                  "document.getElementById('spinner').classList.remove('active');"
                  "document.getElementById('status').innerText='Upload error';};"
                  "xhr.onload=function(){"
                  "document.getElementById('spinner').classList.remove('active');"
                  "document.getElementById('status').innerText='';"
                  "document.getElementById('uploadResult').innerHTML=xhr.responseText;};"
                  "xhr.send(file);}"
                  "document.getElementById('fileInput').addEventListener('change',function(){"
                  "var f=this.files&&this.files[0];sel=null;"
                  "if(f)pick(f);"
                  "else document.getElementById('fwFileName').textContent='No file selected.';});"
                  "</script>",
                  max_size);

    n += snprintf(buf + n, sz - (size_t)n, "<p class=\"small\">%s</p>", timeout_info);
    if ((size_t)n >= sz)
        n = (int)sz - 1;
    scaffold_page_close(buf + n, sz - (size_t)n, "/", NULL);
}

// =============================================================================
// Firmware upload — OTA completion callback + ROUTE_BINARY handler
// =============================================================================

static void firmware_upload_complete(struct tcp_pcb *tpcb, size_t total_written) {
    char msg[1024];
    flash_ota_result_t r = flash_ota_finish();
    const firmware_header_t *h = &r.header;

    if (!r.valid) {
        debug_status("ERROR", "OTA: invalid header (magic mismatch)\n");
        send_response(tpcb, "<h2 style='color:red'>❌ Invalid firmware header (magic)</h2>");
        return;
    }
    if (!r.slot_ok) {
        debug_status("ERROR", "OTA: slot mismatch (header slot=%u)\n", h->slot);
        send_response(tpcb, "<h2 style='color:red'>❌ Slot mismatch — wrong firmware binary</h2>");
        return;
    }
    if (!r.crc_ok) {
        debug_status("ERROR", "OTA: CRC mismatch (expected=0x%08X got=0x%08X)\n", h->crc32,
                     r.actual_crc);
        snprintf(msg, sizeof(msg),
                 "<h2 style='color:red'>CRC MISMATCH: expected 0x%08X, got 0x%08X</h2>", h->crc32,
                 r.actual_crc);
        send_response(tpcb, msg);
        return;
    }

    bool uc_valid = fw_use_case_meta_valid(h);
    bool uc_mismatch = uc_valid && (h->use_case_id != USE_CASE_ID);

    if (uc_mismatch) {
        debug_status("WARN", "OTA: use-case mismatch (uploaded %s/%u, running %s/%u)\n",
                     h->use_case_name, (unsigned)h->use_case_id, USE_CASE_NAME,
                     (unsigned)USE_CASE_ID);
        flash_zero_settings_sector();
    }

    char uc_html[160];
    if (uc_valid) {
        snprintf(uc_html, sizeof(uc_html), "Use Case: <code>%s</code> (id %u)<br>",
                 h->use_case_name, (unsigned)h->use_case_id);
    } else {
        snprintf(uc_html, sizeof(uc_html), "Use Case: <code>legacy/unknown metadata</code><br>");
    }

    char warn_html[448] = "";
    if (uc_mismatch) {
        snprintf(warn_html, sizeof(warn_html),
                 "<p style='color:orange'><b>Warning:</b> Uploaded use case "
                 "(<code>%s</code>, id %u) differs from running use case "
                 "(<code>%s</code>, id %u). "
                 "Use-case settings were reset to 0 and must be reconfigured.</p>",
                 h->use_case_name, (unsigned)h->use_case_id, USE_CASE_NAME, (unsigned)USE_CASE_ID);
    }

    snprintf(msg, sizeof(msg),
             "<h2 style='color:green'>Valid Firmware – you may now reboot!</h2>"
             "<p>Version: <code>%s</code><br>"
             "Build Date: <code>%s</code><br>"
             "Size: <code>%u bytes</code><br>"
             "CRC32: <code>0x%08X</code><br>"
             "Slot: <code>%u</code><br>"
             "%s</p>%s",
             h->git_version, h->build_date, h->firmware_size, h->crc32, h->slot, uc_html,
             warn_html);

    debug_status("OK", "OTA: firmware valid, slot %u marked (%s, %s)\n", h->slot, h->git_version,
                 h->use_case_name);
    send_response(tpcb, msg);
}

// The slot named by the page in its X-Tinta-Slot header, or -1 if it sent none.
static int requested_slot(void) {
    static const char *const names[] = {"X-Tinta-Slot:", "x-tinta-slot:"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        const char *h = strstr(upload_session.header_buffer, names[i]);
        if (!h)
            continue;
        h += strlen(names[i]);
        while (*h == ' ')
            h++;
        return (*h == '0' || *h == '1') ? *h - '0' : -1;
    }
    return -1;
}

void web_firmware_upload(struct tcp_pcb *tpcb, int content_length, const char *body,
                         size_t body_len) {
    if (content_length <= 0 || (size_t)content_length > FIRMWARE_FLASH_SIZE) {
        dlog("UPLOAD FIRMWARE: invalid Content-Length (%d)\n", content_length);
        send_response(tpcb, content_length < 0
                                ? "<h2 style='color:red'>Missing Content-Length</h2>"
                                : "<h2 style='color:red'>Firmware too large or empty</h2>");
        reset_upload_session();
        return;
    }

    dlog("UPLOAD FIRMWARE: Expected length: %d\n", content_length);
    upload_session.expected_length = (size_t)content_length;

    const char *slot_info = get_active_firmware_slot_info();
    uint32_t target_offset = (strncmp(slot_info, "SLOT_0", 6) == 0) ? FIRMWARE_SLOT1_FLASH_OFFSET
                                                                    : FIRMWARE_SLOT0_FLASH_OFFSET;

    // A page left open from an earlier start picked the image for the slot that was free then.
    // It names that slot; refuse before anything is erased if the sign now runs from it.
    const int page_slot = requested_slot();
    const int free_slot = target_offset == FIRMWARE_SLOT1_FLASH_OFFSET ? 1 : 0;
    if (page_slot >= 0 && page_slot != free_slot) {
        debug_status("WARN", "OTA: page chose slot %d, slot %d is free — page out of date\n",
                     page_slot, free_slot);
        send_response(tpcb, "<h2 style='color:red'>This page is out of date</h2>"
                            "<p>Tinta has restarted since it was opened. Reload the page and "
                            "choose the file again. Nothing was changed.</p>");
        reset_upload_session();
        return;
    }

    const size_t erase_length = flash_ota_erase_length((size_t)content_length);

    upload_session.active = true;
    upload_session.total_received = 0;
    upload_session.type = UPLOAD_FIRMWARE;
    upload_session.flash_offset = target_offset;

    dlog("UPLOAD FIRMWARE: target offset 0x%08X (active=%.*s)\n", target_offset, 6, slot_info);

    if (!erase_length) {
        dlog("ERROR: erase_length exceeds FIRMWARE_FLASH_SIZE, aborting\n");
        send_response(tpcb,
                      "<h2 style='color:red'>Firmware upload aborted (erase range invalid)</h2>");
        reset_upload_session();
        return;
    }

    size_t num_sectors = (erase_length + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;
    size_t est_erase_ms = num_sectors * 43U;
    size_t est_write_ms = (((size_t)content_length + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * 2U;
    debug_status("INFO", "Upload: %d bytes, erase ~%dms, write ~%dms, total ~%dms\n",
                 content_length, (int)est_erase_ms, (int)est_write_ms,
                 (int)(est_erase_ms + est_write_ms));

    ota_begin(tpcb, (size_t)content_length, erase_length, target_offset, firmware_upload_complete);

    if (body && body_len > 0) {
        if (body_len > (size_t)content_length)
            body_len = (size_t)content_length;
        ota_stage_data(body, body_len);
    }
}

// =============================================================================
// Firmware demote — developer helper to swap A/B slot on next boot
// =============================================================================

void web_firmware_demote_active(struct tcp_pcb *tpcb) {
    const char *slot_info = get_active_firmware_slot_info();
    uint32_t active_offset = 0;
    uint8_t active_slot = 255;

    char *buf = malloc(8192);
    if (!buf) {
        send_response(tpcb, "<h2>Out of memory</h2>");
        return;
    }

    if (strncmp(slot_info, "SLOT_0", 6) == 0) {
        active_offset = FIRMWARE_SLOT0_FLASH_OFFSET;
        active_slot = 0;
    } else if (strncmp(slot_info, "SLOT_1", 6) == 0) {
        active_offset = FIRMWARE_SLOT1_FLASH_OFFSET;
        active_slot = 1;
    } else {
        build_firmware_update_page(buf, 8192,
                                   "❌ Active firmware is not running from SLOT_0/SLOT_1.");
        send_response(tpcb, buf);
        free(buf);
        return;
    }

    if (!flash_demote_slot_header(active_offset)) {
        build_firmware_update_page(buf, 8192,
                                   "❌ Failed to demote active slot metadata (invalid header).");
        send_response(tpcb, buf);
        free(buf);
        return;
    }

    char msg[640];
    snprintf(msg, sizeof(msg),
             "⚠️ Active slot %u metadata demoted (developer helper). "
             "Current firmware keeps running; next reboot may switch to the other slot. "
             "Rewritten: git_version=v0.0.0-0, build_date=1970-01-01.",
             (unsigned)active_slot);
    build_firmware_update_page(buf, 8192, msg);
    send_response(tpcb, buf);
    free(buf);
}
