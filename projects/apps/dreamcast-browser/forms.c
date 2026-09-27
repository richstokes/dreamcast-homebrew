/* Forms and links: what following a link does, and how a form's fields
   become the request that submits it. */

#include "app.h"

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Field text is ISO-8859-1 for the BIOS font. Servers expect UTF-8, and
   HTML submits line breaks as CRLF. */
void form_encoding(const char *text, char *out, size_t size) {
    size_t n = 0;
    for(; *text && n + 3 < size; ++text) {
        unsigned char c = (unsigned char)*text;
        if(c == '\n') {
            out[n++] = '\r';
            out[n++] = '\n';
        } else if(c >= 0x80) {
            out[n++] = (char)(0xc0 | (c >> 6));
            out[n++] = (char)(0x80 | (c & 0x3f));
        } else {
            out[n++] = (char)c;
        }
    }
    out[n] = 0;
}

static int append_form_pair(char *body, size_t size, size_t *used,
                            const char *name, const char *value) {
    static char name_utf8[160];
    static char value_utf8[MAX_FIELD_VALUE * 2 + 1];
    char *escaped_name;
    char *escaped_value;
    int result = -1;

    form_encoding(name, name_utf8, sizeof(name_utf8));
    form_encoding(value, value_utf8, sizeof(value_utf8));
    escaped_name = curl_easy_escape(NULL, name_utf8, 0);
    escaped_value = curl_easy_escape(NULL, value_utf8, 0);
    if(escaped_name && escaped_value &&
       *used + strlen(escaped_name) + strlen(escaped_value) + 2 < size) {
        *used += (size_t)snprintf(body + *used, size - *used, "%s%s=%s",
                                  *used ? "&" : "", escaped_name, escaped_value);
        result = 0;
    }
    /* Values may be passwords: leave no copies behind. */
    memset(value_utf8, 0, sizeof(value_utf8));
    if(escaped_value) memset(escaped_value, 0, strlen(escaped_value));
    curl_free(escaped_name);
    curl_free(escaped_value);
    return result;
}

/* Encodes the form that field_index submits. Returns 0, or -1 after
   setting the status line to explain the refusal. */
int build_form_body(int field_index, char *body, size_t size, size_t *out_used) {
    browser_field_t *clicked = &document.fields[field_index];
    browser_form_t *form = &document.forms[clicked->form];
    size_t used = 0;
    int i;

    if(!form->valid || (form->post && !network_same_origin(current_url, form->action)) ||
       strncmp(form->action, "https://", 8)) {
        snprintf(status_text, sizeof(status_text),
                 "Form blocked: supported HTTPS required; POST must stay on site");
        redraw_needed = 1;
        return -1;
    }
    body[0] = 0;
    for(i = 0; i < document.field_count; ++i) {
        browser_field_t *field = &document.fields[i];
        int ok = 0;
        if(field->form != clicked->form || field->disabled ||
           ((!strcmp(field->type, "checkbox") || !strcmp(field->type, "radio")) &&
            !field->checked) ||
           (document_field_is_submit(field) && i != field_index) ||
           (!strcmp(field->type, "select") && !field->option_count))
            continue;
        if(!form->post && !strcmp(field->type, "password")) {
            snprintf(status_text, sizeof(status_text), "Password forms require POST");
            redraw_needed = 1;
            memset(body, 0, size);
            return -1;
        }
        if(!strcmp(field->type, "image")) {
            /* Image buttons submit the click position; there is none here. */
            char x[80], y[80];
            snprintf(x, sizeof(x), "%s%sx", field->name, field->name[0] ? "." : "");
            snprintf(y, sizeof(y), "%s%sy", field->name, field->name[0] ? "." : "");
            ok = append_form_pair(body, size, &used, x, "0") == 0 &&
                 append_form_pair(body, size, &used, y, "0") == 0;
        } else if(!field->name[0]) {
            continue;
        } else {
            ok = append_form_pair(body, size, &used, field->name, field->value) == 0;
        }
        if(!ok) {
            snprintf(status_text, sizeof(status_text), "Form data too large to send");
            redraw_needed = 1;
            memset(body, 0, size);
            return -1;
        }
    }
    *out_used = used;
    return 0;
}

void submit_form(int field_index) {
    /* Static so a large body does not sit on the stack through TLS. */
    static char body[32768];
    browser_form_t *form = &document.forms[document.fields[field_index].form];
    char target[MAX_URL];
    size_t used;

    if(build_form_body(field_index, body, sizeof(body), &used) < 0) return;
    snprintf(target, sizeof(target), "%s", form->action);
    if(form->post) {
        navigate_request(target, body);
    } else {
        char *query = strpbrk(target, "?#");
        if(query) *query = 0;
        if(strlen(target) + used + 2 >= sizeof(target)) {
            snprintf(status_text, sizeof(status_text), "Form query too long");
            redraw_needed = 1;
        } else {
            strcat(target, "?");
            strcat(target, body);
            navigate_to(target);
        }
    }
    memset(body, 0, sizeof(body));
}

void begin_field_edit(int index, int want_osk) {
    browser_field_t *field = &document.fields[index];
    editing_field = index;
    if(!strcmp(field->type, "select")) {
        select_backup = field->selected;
        field->caret = 0;
        snprintf(status_text, sizeof(status_text), "Arrows choose, Enter done, Esc undo");
    } else {
        snprintf(field_backup, sizeof(field_backup), "%s", field->value);
        field->caret = (int)strlen(field->value);
        snprintf(status_text, sizeof(status_text), "Edit %.14s: Enter done, Ctrl+Enter submits",
                 field->name);
        if(want_osk) open_osk();
    }
    document_refresh_field(&document, index);
    redraw_needed = 1;
}

static void activate_field(int index, int want_osk) {
    browser_field_t *field;
    if(index < 0 || index >= document.field_count) return;
    field = &document.fields[index];
    if(document_field_is_submit(field)) submit_form(index);
    else if(!strcmp(field->type, "checkbox") || !strcmp(field->type, "radio"))
        document_toggle_field(&document, index);
    else begin_field_edit(index, want_osk);
    redraw_needed = 1;
}

void follow_link(int link_id, int want_osk) {
    const char *link;
    if(link_id < 0 || link_id >= document.link_count) return;
    link = document.links[link_id];
    if(!strncmp(link, "form:", 5)) activate_field(atoi(link + 5), want_osk);
    else if(!strncmp(link, "about:", 6)) handle_internal_link(link);
    else navigate_to(link);
}
