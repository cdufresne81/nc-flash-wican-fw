/* Host tests for the auto_pid.json parser -- components/autopid/autopid_config.c, compiled
 * UNMODIFIED against the stub ESP-IDF headers in tools/hosttest/stubs/ and real cJSON.
 *
 * This is the file poll_log's whole PID table comes from, on every boot and on every live
 * reload (issue #39), and the issue #148 cleanup cut fields, keys and a function signature
 * out of it. Nothing here had ever been executed off-device. What it must keep doing:
 *
 *   P1  A file written by OLDER firmware or the web UI, still carrying every retired key
 *       (Period/period, Class/class, sensor_type, type, header/ReceiveHeader, Init,
 *       initialisation, car_model, ecu_protocol, disable_on_sleep_voltage,
 *       pid_polling_min_voltage, cycle, pid_validation), still loads.
 *   P2  ...and yields EXACTLY the table the same file without those keys yields: a retired
 *       key is ignored, never an error and never a value leaking into a live field. If it
 *       fails, an upgraded car logs different channels (or none) than it did before the
 *       update, from a file nobody touched.
 *   P3  That table is the one the file describes (cmd bytes, names, expressions, units,
 *       limits, enabled flags, SampleEvery, Mode, CAN filters, calculated channels) -- so
 *       P2 cannot pass by both files parsing to nothing.
 *   P4  main/default_auto_pid.json, the list seeded on a fresh device, parses to a full
 *       table in which every enabled PID has a pollable command. A seed that fails here
 *       boots a new unit that logs nothing, or silently skips a channel: poll_log passes
 *       over a PID with no command and nothing reports it.
 *   P5  Hostile or damaged files (non-string values where strings belong, "pids" as an
 *       object, a truncated file, no file, more calculated channels than the cap) never
 *       crash or overrun the table and give the fallback the parser documents. Since
 *       issue #39 this parse runs on the poll task mid-drive: a crash here is a reboot loop
 *       on the road.
 *   P6  The NC Flash temporary channel list (/datalog_session) parses to the same table as
 *       the file, says it came from the session (with its token), refuses a list that will
 *       not parse (so a reload keeps the running table), and clearing it brings the file back.
 *   P7  autopid_config_deep_free() releases every table above. Built with
 *       -fsanitize=address,undefined,float-cast-overflow (tools/hosttest/run.sh), so a leak,
 *       overflow or UB anywhere in the parser fails the run -- including the out-of-range
 *       float-to-integer cast its SampleEvery clamp exists to avoid.
 *   P8  A car_data.json left by older firmware (only a factory reset deletes it) appends its
 *       rows and vehicle-specific filters after auto_pid.json's, its retired keys (init,
 *       pid_init, class, period) ignored, and a damaged one neither crashes nor adds an
 *       enabled channel.
 *
 * Fixtures (tools/hosttest/fixtures/): auto_pid_legacy.json is shaped like what older
 * firmware and the web UI's storeAutoTableData() wrote, with the retired keys in it, some
 * with odd types (numbers, null, bools) as hand edits leave them; auto_pid_stripped.json is
 * the SAME file with exactly those keys removed. Edit the two together.
 *
 * Usage: autopid_config_test <fixtures dir> <main/default_auto_pid.json>
 * Build + run: tools/hosttest/run.sh
 */

#define _GNU_SOURCE   /* mkdtemp */
#include <float.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "freertos/FreeRTOS.h"   /* autopid.h names SemaphoreHandle_t without including it */
#include "autopid_config.h"
#include "obd2_standard_pids.h"

static int g_failures = 0;
static int g_checks   = 0;

#define CHECK(cond, ...)                                                        \
    do {                                                                        \
        g_checks++;                                                             \
        if (!(cond)) {                                                          \
            g_failures++;                                                       \
            printf("  not ok %d - ", g_checks);                                 \
            printf(__VA_ARGS__);                                                \
            printf("\n         at %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                       \
    } while (0)

/* ---- What autopid_config.c links against outside itself ------------------------------ */

/* The autopid file lock lives in autopid.c (a FreeRTOS mutex). Single-threaded here, so
 * it is always free. */
bool autopid_file_trylock(uint32_t wait_ms) { (void)wait_ms; return true; }
void autopid_file_unlock(void) {}

/* get_pid_from_string() lives in autopid.c, which drags in the whole autopid runtime
 * (tasks, MQTT, csv_logger). This is a VERBATIM copy of it, run over the REAL standard-PID
 * table (main/obd2_standard_pids.h). run.sh diffs the two and fails the run if they drift,
 * so if autopid.c's version changes, paste the new one here unchanged. */
const std_pid_t *get_pid_from_string(const char *pid_string)
{
    char pid_hex[3];
    uint8_t pid_value;

    // Extract first 2 characters (hex PID)
    strncpy(pid_hex, pid_string, 2);
    pid_hex[2] = '\0';

    // Convert hex string to integer
    pid_value = (uint8_t)strtol(pid_hex, NULL, 16);

    // Get the base PID info
    const std_pid_t *pid_info = get_pid(pid_value);
    if (!pid_info)
    {
        return NULL;
    }

    // If there's a parameter name specified after the dash
    if (strchr(pid_string, '-'))
    {
        const char *param_name = strchr(pid_string, '-') + 1;

        // For all PIDs, verify the parameter exists
        bool param_found = false;
        for (int i = 0; i < pid_info->num_params; i++)
        {
            if (strcmp(pid_info->params[i].name, param_name) == 0)
            {
                param_found = true;
                break;
            }
        }
        if (!param_found)
        {
            return NULL;
        }
    }

    return pid_info;
}

/* ---- The "filesystem" ----------------------------------------------------------------
 * The stub hw_config.h makes FS_MOUNT_POINT ".", and main() chdir()s into a fresh temp
 * directory, so these helpers decide exactly what the parser finds there. */

static char *read_whole(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (n >= 0) ? malloc((size_t)n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n)
    {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf)
    {
        buf[n] = '\0';
        if (len_out)
            *len_out = (size_t)n;
    }
    return buf;
}

static void put_file(const char *name, const char *json)
{
    FILE *f = fopen(name, "wb");
    if (!f || fputs(json, f) < 0)
    {
        printf("FAIL: cannot write %s in the temp dir\n", name);
        exit(1);
    }
    fclose(f);
}

static void put_auto_pid(const char *json) { put_file("auto_pid.json", json); }

static char *fixture(const char *dir, const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    char *s = read_whole(path, NULL);
    if (!s)
    {
        printf("FAIL: cannot read fixture %s\n", path);
        exit(1);
    }
    return s;
}

/* Write `json` as auto_pid.json and load it, exactly as boot / a live reload does. */
static autopid_config_t *load_json(const char *json)
{
    put_auto_pid(json);
    return load_autopid_config();
}

/* ---- Deep table comparison (P2, P6) ---------------------------------------------------
 * Every field the parser sets. Returns false and describes the FIRST difference. */

static bool str_eq(const char *a, const char *b)
{
    return (a == NULL || b == NULL) ? (a == b) : (strcmp(a, b) == 0);
}

static const char *show(const char *s) { return s ? s : "(null)"; }

static bool same_param(const parameter_t *a, const parameter_t *b, const char *where,
                       char *why, size_t whylen)
{
#define PDIFF(field, fmt, va, vb)                                                     \
    do { snprintf(why, whylen, "%s." field ": " fmt " vs " fmt, where, va, vb); return false; } while (0)
    if (!str_eq(a->name, b->name))             PDIFF("name", "\"%s\"", show(a->name), show(b->name));
    if (!str_eq(a->expression, b->expression)) PDIFF("expression", "\"%s\"", show(a->expression), show(b->expression));
    if (!str_eq(a->unit, b->unit))             PDIFF("unit", "\"%s\"", show(a->unit), show(b->unit));
    if (a->enabled != b->enabled)              PDIFF("enabled", "%d", a->enabled, b->enabled);
    if (a->min != b->min)                      PDIFF("min", "%g", (double)a->min, (double)b->min);
    if (a->max != b->max)                      PDIFF("max", "%g", (double)a->max, (double)b->max);
    if (a->timer != b->timer)                  PDIFF("timer", "%lld", (long long)a->timer, (long long)b->timer);
    if (a->value != b->value)                  PDIFF("value", "%g", (double)a->value, (double)b->value);
#undef PDIFF
    return true;
}

static bool same_params(const parameter_t *a, uint32_t na, const parameter_t *b, uint32_t nb,
                        const char *where, char *why, size_t whylen)
{
    if (na != nb)
    {
        snprintf(why, whylen, "%s: parameter count %u vs %u", where, (unsigned)na, (unsigned)nb);
        return false;
    }
    if ((a == NULL) != (b == NULL))
    {
        snprintf(why, whylen, "%s: parameter array %s vs %s", where, a ? "set" : "NULL", b ? "set" : "NULL");
        return false;
    }
    for (uint32_t i = 0; a && i < na; i++)
    {
        char w[96];
        snprintf(w, sizeof(w), "%s[%u]", where, (unsigned)i);
        if (!same_param(&a[i], &b[i], w, why, whylen))
            return false;
    }
    return true;
}

static bool same_table(const autopid_config_t *a, const autopid_config_t *b, bool check_source,
                       char *why, size_t whylen)
{
#define TDIFF(...) do { snprintf(why, whylen, __VA_ARGS__); return false; } while (0)
    if (!a || !b)                                  TDIFF("a table is NULL");
    if (a->pid_count != b->pid_count)              TDIFF("pid_count %u vs %u", (unsigned)a->pid_count, (unsigned)b->pid_count);
    if (a->pid_std_en != b->pid_std_en)            TDIFF("pid_std_en %d vs %d", a->pid_std_en, b->pid_std_en);
    if (a->pid_custom_en != b->pid_custom_en)      TDIFF("pid_custom_en %d vs %d", a->pid_custom_en, b->pid_custom_en);
    if (a->pid_specific_en != b->pid_specific_en)  TDIFF("pid_specific_en %d vs %d", a->pid_specific_en, b->pid_specific_en);
    if (check_source && a->from_session_list != b->from_session_list)
        TDIFF("from_session_list %d vs %d", a->from_session_list, b->from_session_list);
    if (check_source && a->session_token != b->session_token)
        TDIFF("session_token %u vs %u", (unsigned)a->session_token, (unsigned)b->session_token);

    for (uint32_t i = 0; i < a->pid_count; i++)
    {
        const pid_data_t *pa = &a->pids[i], *pb = &b->pids[i];
        char w[64];
        snprintf(w, sizeof(w), "pids[%u]", (unsigned)i);
        if (!str_eq(pa->cmd, pb->cmd))         TDIFF("%s.cmd \"%s\" vs \"%s\"", w, show(pa->cmd), show(pb->cmd));
        if (pa->pid_type != pb->pid_type)      TDIFF("%s.pid_type %d vs %d", w, pa->pid_type, pb->pid_type);
        if (pa->enabled != pb->enabled)        TDIFF("%s.enabled %d vs %d", w, pa->enabled, pb->enabled);
        if (pa->mode != pb->mode)              TDIFF("%s.mode 0x%02X vs 0x%02X", w, pa->mode, pb->mode);
        if (pa->sample_every != pb->sample_every) TDIFF("%s.sample_every %u vs %u", w, pa->sample_every, pb->sample_every);
        if (pa->sample_ctr != pb->sample_ctr)  TDIFF("%s.sample_ctr %u vs %u", w, pa->sample_ctr, pb->sample_ctr);
        if (!same_params(pa->parameters, pa->parameters_count, pb->parameters, pb->parameters_count, w, why, whylen))
            return false;
    }

    if (a->can_filters_count != b->can_filters_count)
        TDIFF("can_filters_count %u vs %u", (unsigned)a->can_filters_count, (unsigned)b->can_filters_count);
    for (uint32_t i = 0; i < a->can_filters_count; i++)
    {
        const can_filter_t *fa = &a->can_filters[i], *fb = &b->can_filters[i];
        char w[64];
        snprintf(w, sizeof(w), "can_filters[%u]", (unsigned)i);
        if (fa->frame_id != fb->frame_id)       TDIFF("%s.frame_id 0x%X vs 0x%X", w, (unsigned)fa->frame_id, (unsigned)fb->frame_id);
        if (fa->is_extended != fb->is_extended) TDIFF("%s.is_extended %d vs %d", w, fa->is_extended, fb->is_extended);
        if (fa->is_vehicle_specific != fb->is_vehicle_specific)
            TDIFF("%s.is_vehicle_specific %d vs %d", w, fa->is_vehicle_specific, fb->is_vehicle_specific);
        if (!same_params(fa->parameters, fa->parameters_count, fb->parameters, fb->parameters_count, w, why, whylen))
            return false;
    }

    if (!same_params(a->calculated, a->calculated_count, b->calculated, b->calculated_count,
                     "calculated", why, whylen))
        return false;
#undef TDIFF
    return true;
}

/* ---- Small assertion helpers ---------------------------------------------------------- */

static const parameter_t *p0(const autopid_config_t *c, uint32_t i)
{
    return (c && i < c->pid_count && c->pids[i].parameters_count >= 1) ? &c->pids[i].parameters[0] : NULL;
}

static bool is_empty_table(const autopid_config_t *c)
{
    return c && c->pid_count == 0 && c->pids == NULL && c->can_filters_count == 0 &&
           c->can_filters == NULL && c->calculated_count == 0 && c->calculated == NULL &&
           !c->from_session_list;
}

/* Number of elements of top-level array `key` in `doc`, counted by cJSON directly -- an
 * independent count to hold the parser's against. -1 if absent / not an array. */
static int json_array_len(const char *doc, const char *key)
{
    cJSON *root = cJSON_Parse(doc);
    cJSON *arr = root ? cJSON_GetObjectItemCaseSensitive(root, key) : NULL;
    int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : -1;
    cJSON_Delete(root);
    return n;
}

/* --------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        printf("usage: %s <fixtures dir> <default_auto_pid.json>\n", argv[0]);
        return 2;
    }
    const char *fixtures = argv[1];
    char *legacy_json   = fixture(fixtures, "auto_pid_legacy.json");
    char *stripped_json = fixture(fixtures, "auto_pid_stripped.json");
    size_t default_len  = 0;
    char *default_json  = read_whole(argv[2], &default_len);
    if (!default_json)
    {
        printf("FAIL: cannot read %s\n", argv[2]);
        return 1;
    }

    const char *tmproot = getenv("TMPDIR");
    char tmpl[512];
    snprintf(tmpl, sizeof(tmpl), "%s/autopid_config_test.XXXXXX", (tmproot && tmproot[0]) ? tmproot : "/tmp");
    const char *tmpdir = mkdtemp(tmpl);
    if (!tmpdir || chdir(tmpdir) != 0)
    {
        printf("FAIL: cannot create/enter a temp dir\n");
        return 1;
    }

    char why[256];

    printf("# P1 a file carrying every retired key still loads\n");
    autopid_config_t *legacy = load_json(legacy_json);
    CHECK(legacy != NULL, "legacy auto_pid.json -> non-NULL table");

    printf("# P3 the stripped file parses to the table it describes\n");
    autopid_config_t *stripped = load_json(stripped_json);
    CHECK(stripped != NULL, "stripped auto_pid.json -> non-NULL table");
    if (stripped)
    {
        const autopid_config_t *c = stripped;
        CHECK(c->pid_count == 5, "3 custom pids + 2 std pids, got %u", (unsigned)c->pid_count);
        CHECK(c->pid_std_en && c->pid_custom_en && !c->pid_specific_en, "standard_pids/pids on, car_specific off");
        CHECK(!c->from_session_list && c->session_token == 0, "table came from the file");
        if (c->pid_count == 5)
        {
            const parameter_t *rpm = p0(c, 0), *knock = p0(c, 1), *aat = p0(c, 2), *std = p0(c, 3), *vss = p0(c, 4);
            CHECK(c->pids[0].pid_type == PID_CUSTOM && str_eq(c->pids[0].cmd, "010C1\r"),
                  "RPM cmd is the PID text + CR (poll_log sends it verbatim): \"%s\"", show(c->pids[0].cmd));
            CHECK(c->pids[0].enabled && c->pids[0].mode == 0x01 && c->pids[0].sample_every == 0,
                  "RPM enabled, mode 01, every sweep");
            CHECK(rpm && str_eq(rpm->name, "RPM") && str_eq(rpm->expression, "((B3*256)+B4)/4") &&
                  str_eq(rpm->unit, "rpm") && rpm->enabled,
                  "RPM name/expression/unit (\"Unit\" matches the parser's \"unit\": cJSON lookups ignore case)");
            CHECK(rpm && rpm->min == 0.0f && rpm->max == 9000.0f && rpm->value == FLT_MAX,
                  "RPM MinValue/MaxValue from strings, value starts unset");

            CHECK(str_eq(c->pids[1].cmd, "2210741\r") && c->pids[1].mode == 0x22,
                  "KNOCKR is a Mode 22 request");
            CHECK(!c->pids[1].enabled && knock && !knock->enabled, "KNOCKR \"enabled\": false holds for row and channel");
            CHECK(c->pids[1].sample_every == 4, "KNOCKR SampleEvery \"4\" (string) -> 4, got %u", c->pids[1].sample_every);
            CHECK(knock && knock->min == FLT_MAX && knock->max == FLT_MAX, "empty MinValue/MaxValue -> no limit");

            CHECK(str_eq(c->pids[2].cmd, "01461\r") && c->pids[2].mode == 0x01 && c->pids[2].sample_every == 16,
                  "AAT: Mode derived from the PID prefix when absent, SampleEvery 16");
            CHECK(aat && aat->min == -40.0f && aat->max == 215.0f, "numeric MinValue/MaxValue");

            CHECK(c->pids[3].pid_type == PID_STD && str_eq(c->pids[3].cmd, "010C\r"),
                  "std pid 0C-EngineRPM -> cmd 010C, got \"%s\"", show(c->pids[3].cmd));
            CHECK(std && str_eq(std->name, "0C-EngineRPM") && str_eq(std->unit, "rpm") &&
                  str_eq(std->expression, "none"),
                  "std pid unit comes from the standard-PID table");
            CHECK(c->pids[3].sample_every == 2 && c->pids[3].mode == 0x01, "std pid SampleEvery 2, mode 01");
            CHECK(c->pids[3].enabled, "std pid \"enabled\": true");
            /* The web UI passes std_pids through verbatim, so a std pid the user switched off
             * arrives exactly like this. If "enabled" is lost here, poll_log polls a PID the
             * user turned off and spends bus time on it every sweep. */
            CHECK(c->pids[4].pid_type == PID_STD && !c->pids[4].enabled,
                  "std pid 0D-VehicleSpeed \"enabled\": false stays off");
            CHECK(str_eq(c->pids[4].cmd, "010D\r") && vss && str_eq(vss->unit, "km/h"),
                  "disabled std pid still resolves (cmd \"%s\")", show(c->pids[4].cmd));
        }

        CHECK(c->can_filters_count == 2, "2 CAN filters, got %u", (unsigned)c->can_filters_count);
        if (c->can_filters_count == 2)
        {
            const can_filter_t *f0 = &c->can_filters[0], *f1 = &c->can_filters[1];
            CHECK(f0->frame_id == 0x201 && !f0->is_extended && !f0->is_vehicle_specific && f0->parameters_count == 2,
                  "filter 0: numeric frame_id 513, standard id, two channels");
            CHECK(f1->frame_id == 0x18DAF110 && f1->is_extended && f1->parameters_count == 1,
                  "filter 1: \"0x18DAF110\" string id -> extended");
            if (f0->parameters_count == 2 && f1->parameters_count == 1)
            {
                const parameter_t *sp = &f0->parameters[0], *br = &f0->parameters[1];
                CHECK(str_eq(sp->name, "SPEED") && str_eq(sp->expression, "((B0*256)+B1)/100") &&
                      str_eq(sp->unit, "km/h") && sp->min == 0.0f && sp->max == 300.0f && sp->enabled,
                      "SPEED channel");
                CHECK(str_eq(br->name, "BRAKE") && !br->enabled && str_eq(br->unit, "none") &&
                      br->min == FLT_MAX, "BRAKE: disabled, empty unit -> \"none\", no limit");
                CHECK(str_eq(f1->parameters[0].name, "XID") && f1->parameters[0].enabled,
                      "XID: absent \"enabled\" means enabled");
            }
        }

        CHECK(c->calculated_count == 2, "2 calculated channels, got %u", (unsigned)c->calculated_count);
        if (c->calculated_count == 2)
        {
            CHECK(str_eq(c->calculated[0].name, "BOOST") && str_eq(c->calculated[0].expression, "(MAP-BARO)*0.145038") &&
                  str_eq(c->calculated[0].unit, "PSI") && c->calculated[0].enabled, "BOOST");
            CHECK(str_eq(c->calculated[1].name, "AFR") && !c->calculated[1].enabled &&
                  str_eq(c->calculated[1].unit, "none"), "AFR disabled, empty unit -> \"none\"");
        }
    }

    printf("# P2 retired keys are ignored: legacy table == stripped table\n");
    why[0] = '\0';
    CHECK(same_table(legacy, stripped, true, why, sizeof(why)), "first difference: %s", why);

    printf("# P4 the seeded default list parses to a full, pollable table\n");
    autopid_config_t *def = load_json(default_json);
    int def_pids = json_array_len(default_json, "pids");
    int def_std  = json_array_len(default_json, "std_pids");
    int def_flt  = json_array_len(default_json, "can_filters");
    int def_calc = json_array_len(default_json, "calculated");
    int def_total = (def_pids > 0 ? def_pids : 0) + (def_std > 0 ? def_std : 0);
    /* Floors, so a default list gutted by mistake fails here rather than passing on an
     * empty count. Raise them if the shipped list grows; lower only on purpose. */
    CHECK(def_pids >= 10, "default list has >= 10 polled PIDs (has %d)", def_pids);
    CHECK(def_flt >= 1, "default list has broadcast CAN filters (has %d)", def_flt);
    CHECK(def_calc >= 1, "default list has calculated channels (has %d)", def_calc);
    CHECK(def != NULL, "default list -> non-NULL table");
    if (def)
    {
        CHECK((int)def->pid_count == def_total, "every listed PID is in the table: %u vs %d",
              (unsigned)def->pid_count, def_total);
        CHECK((int)def->can_filters_count == def_flt, "every CAN filter is in the table: %u vs %d",
              (unsigned)def->can_filters_count, def_flt);
        CHECK((int)def->calculated_count == (def_calc > 24 ? 24 : def_calc),
              "every calculated channel is in the table: %u vs %d", (unsigned)def->calculated_count, def_calc);
        int enabled = 0, pollable = 0;
        for (uint32_t i = 0; i < def->pid_count; i++)
        {
            const pid_data_t *p = &def->pids[i];
            if (!p->enabled)
                continue;
            enabled++;
            size_t n = p->cmd ? strlen(p->cmd) : 0;
            /* poll_log skips any PID whose cmd is NULL (polllog_poll_one returns on
             * !pid->cmd) and autopid_reload_config() deliberately accepts such a table, so
             * a seeded PID with no command just never logs and nothing reports it. Same for
             * a parameter array that does not match its count. Pin it on the seed instead. */
            bool ok = n >= 3 && p->cmd[n - 1] == '\r' && p->parameters_count == 1 && p->parameters &&
                      p->parameters[0].name && p->parameters[0].expression;
            if (ok)
                pollable++;
            else
                CHECK(ok, "default pids[%u] (\"%s\") is enabled but not pollable", (unsigned)i,
                      p->parameters ? show(p->parameters[0].name) : "?");
        }
        CHECK(enabled >= 10 && pollable == enabled, "%d of %d enabled default PIDs pollable", pollable, enabled);
    }

    printf("# P5 damaged and hostile files\n");
    /* No file at all: a fresh filesystem, or one the seed has not written yet. */
    remove("auto_pid.json");
    autopid_config_t *none = load_autopid_config();
    CHECK(is_empty_table(none), "missing auto_pid.json -> an empty table, not NULL");

    /* A truncated write (power cut mid-save). */
    autopid_config_t *trunc = load_json("{\"pids\": [ {\"Name\": \"RPM\", \"PID\": \"010C1\"");
    CHECK(is_empty_table(trunc), "truncated auto_pid.json -> an empty table");

    /* "pids" as an object: the sizing pass skips it, so parsing its children would write
     * past pids[] (ASan reports it). Only the std pid may land in the table. */
    autopid_config_t *obj = load_json(
        "{\"pids\": {\"a\": {\"PID\": \"010C1\"}, \"b\": {\"PID\": \"01051\"}, \"c\": {\"PID\": \"01461\"}},"
        " \"std_pids\": [ {\"Name\": \"0C-EngineRPM\"} ]}");
    CHECK(obj && obj->pid_count == 1 && obj->pids[0].pid_type == PID_STD && !obj->pid_custom_en,
          "\"pids\" as an object is ignored, std_pids still load");

    /* Wrong JSON types everywhere a string, number, array or object is expected. */
    autopid_config_t *bad = load_json(
        "{\"standard_pids\": 123, \"car_specific\": [],"
        " \"pids\": ["
        "   {\"Name\": 7, \"PID\": 42, \"Expression\": null, \"Unit\": {}, \"MinValue\": true, \"MaxValue\": [],"
        "    \"SampleEvery\": \"abc\", \"Mode\": 22, \"enabled\": \"yes\"},"
        "   {\"Name\": \"ONE\", \"PID\": \"0\", \"SampleEvery\": 1e20, \"Mode\": \"22\"},"
        "   {\"Name\": \"MISMATCH\", \"PID\": \"220C00\", \"SampleEvery\": -3, \"Mode\": \"01\"}"
        " ],"
        " \"std_pids\": [ {\"Name\": 12}, {\"Name\": \"0C\"}, {\"Name\": \"0C-NoSuchParam\"} ],"
        " \"can_filters\": [ {\"frame_id\": \"zz\"}, {\"frame_id\": -1}, \"str\","
        "   {\"frame_id\": 291, \"parameters\": [5, {\"name\": 1, \"min\": \"12.5\"}]},"
        "   {\"frame_id\": 292, \"parameters\": {\"name\": \"obj\"}} ],"
        " \"calculated\": [7, {\"name\": \"X\"}, \"y\"]}");
    CHECK(bad != NULL, "wrong-typed values -> a table, not a crash");
    if (bad)
    {
        CHECK(!bad->pid_std_en && !bad->pid_specific_en, "non-string standard_pids/car_specific -> off");
        CHECK(bad->pid_count == 6, "3 custom + 3 std entries kept, got %u", (unsigned)bad->pid_count);
        if (bad->pid_count == 6)
        {
            const parameter_t *q = p0(bad, 0);
            CHECK(bad->pids[0].cmd == NULL, "numeric PID -> no command (never strlen(NULL))");
            CHECK(q && str_eq(q->name, "none") && str_eq(q->expression, "none") && str_eq(q->unit, "none"),
                  "non-string Name/Expression/Unit -> \"none\"");
            CHECK(q && q->min == FLT_MAX && q->max == FLT_MAX, "non-numeric MinValue/MaxValue -> no limit");
            CHECK(bad->pids[0].sample_every == 0 && bad->pids[0].mode == 0x01 && bad->pids[0].enabled,
                  "garbage SampleEvery -> every sweep, numeric Mode -> derived 01, non-bool enabled -> on");
            CHECK(bad->pids[1].cmd && bad->pids[1].cmd[0] == '\0',
                  "one-character PID -> an empty (unpollable) command, not heap garbage");
            CHECK(bad->pids[1].sample_every == AUTOPID_MAX_SAMPLE_EVERY, "SampleEvery 1e20 clamped to %d, got %u",
                  AUTOPID_MAX_SAMPLE_EVERY, bad->pids[1].sample_every);
            CHECK(bad->pids[2].mode == 0x22 && bad->pids[2].sample_every == 0,
                  "stored Mode contradicting the PID prefix loses to the prefix; negative SampleEvery -> 0");
            CHECK(bad->pids[3].pid_type == PID_STD && bad->pids[3].cmd == NULL && p0(bad, 3) &&
                  str_eq(p0(bad, 3)->name, "none"), "numeric std pid Name -> no command");
            CHECK(bad->pids[4].cmd == NULL, "std pid \"0C\" names no parameter -> no command");
            CHECK(bad->pids[5].cmd == NULL && p0(bad, 5) && str_eq(p0(bad, 5)->unit, "none"),
                  "std pid with an unknown parameter -> no command");
        }
        CHECK(bad->can_filters_count == 5, "every can_filters slot kept, got %u", (unsigned)bad->can_filters_count);
        if (bad->can_filters_count == 5)
        {
            CHECK(bad->can_filters[0].frame_id == 0 && bad->can_filters[0].parameters == NULL &&
                  bad->can_filters[1].parameters == NULL && bad->can_filters[2].parameters == NULL,
                  "unparseable frame_id / non-object filter -> empty slot");
            const can_filter_t *f = &bad->can_filters[3];
            CHECK(f->frame_id == 291 && f->parameters_count == 2 && f->parameters &&
                  !f->parameters[0].enabled,
                  "non-object channel skipped: never an enabled channel");
            CHECK(f->parameters_count == 2 && f->parameters &&
                  str_eq(f->parameters[1].name, "none") && f->parameters[1].min == 12.5f,
                  "numeric name -> \"none\", string min parsed");
            CHECK(bad->can_filters[4].frame_id == 292 && bad->can_filters[4].parameters == NULL &&
                  bad->can_filters[4].parameters_count == 0, "\"parameters\" as an object -> no channels");
        }
        CHECK(bad->calculated_count == 1 && str_eq(bad->calculated[0].name, "X") &&
              str_eq(bad->calculated[0].expression, "none"), "non-object calculated entries skipped");
    }

    /* The calculated-channel cap (AUTOPID_MAX_CALCULATED, 24): every channel is evaluated
     * every sweep on the poll task, and the source calls the bound a brick invariant -- an
     * oversized list must not exhaust the heap. 30 entries in, the first 24 out. */
    char big[2048];
    size_t bl = (size_t)snprintf(big, sizeof(big), "{\"pids\": [{\"Name\": \"RPM\", \"PID\": \"010C1\"}], \"calculated\": [");
    for (int i = 0; i < 30; i++)
        bl += (size_t)snprintf(big + bl, sizeof(big) - bl, "%s{\"name\": \"C%d\"}", i ? "," : "", i);
    snprintf(big + bl, sizeof(big) - bl, "]}");
    autopid_config_t *capped = load_json(big);
    CHECK(capped && capped->calculated_count == 24 && str_eq(capped->calculated[23].name, "C23"),
          "30 calculated channels capped at the first 24, got %u", capped ? (unsigned)capped->calculated_count : 0u);

    printf("# P6 the NC Flash temporary channel list\n");
    put_auto_pid(legacy_json);
    size_t slen = strlen(stripped_json);
    /* Not NUL-terminated on purpose: the HTTP body it comes from is not either. */
    char *body = malloc(slen);
    memcpy(body, stripped_json, slen);
    CHECK(autopid_session_list_set(body, slen, 77, 0) == ESP_OK && autopid_session_list_active(),
          "session list set");
    free(body);
    autopid_config_t *sess = load_autopid_config();
    CHECK(sess && sess->from_session_list && sess->session_token == 77,
          "the table says it came from session 77");
    why[0] = '\0';
    CHECK(same_table(sess, stripped, false, why, sizeof(why)),
          "session list parses like the same file; first difference: %s", why);

    const char *broken = "{\"pids\": [";
    CHECK(autopid_session_list_set(broken, strlen(broken), 78, 0) == ESP_OK, "broken session list set");
    autopid_config_t *sess_bad = load_autopid_config();
    CHECK(sess_bad == NULL, "a session list that will not parse fails the load (reload keeps its table)");

    CHECK(autopid_session_list_set(NULL, 0, 0, 0) == ESP_OK && !autopid_session_list_active(),
          "session list cleared");
    autopid_config_t *after = load_autopid_config();
    why[0] = '\0';
    CHECK(after && !after->from_session_list && same_table(after, legacy, true, why, sizeof(why)),
          "cleared -> the file again; first difference: %s", why);

    printf("# P8 car_data.json left on an upgraded device\n");
    /* Current firmware never writes car_data.json, but every load still reads it and only a
     * factory reset deletes it (main/filesystem.c), so a device upgraded from the old
     * vehicle-profile firmware still has one. Its rows and filters are APPENDED to the
     * auto_pid.json table; its retired keys (init, pid_init, class, period, car_model) must
     * be ignored like auto_pid.json's, and a damaged one must not take the table down. */
    static const char car_legacy[] =
        "{\"cars\": [{\"car_model\": \"Mazda: MX-5 NC\", \"init\": \"ATSP6;ATSH7E0;\","
        "  \"can_filters\": [{\"frame_id\": 1056, \"parameters\": [{\"name\": \"GEAR\","
        "     \"expression\": \"B0&7\", \"unit\": \"\", \"class\": \"none\", \"period\": \"100\"}]}],"
        "  \"pids\": [{\"pid\": \"2101\", \"pid_init\": \"ATSH7E0;\", \"enabled\": true,"
        "     \"parameters\": [{\"name\": \"OILT\", \"expression\": \"B4-40\", \"unit\": \"C\","
        "        \"class\": \"temperature\", \"period\": \"1000\", \"min\": \"-40\", \"max\": \"150\"}]}]}]}";
    static const char car_stripped[] =
        "{\"cars\": [{"
        "  \"can_filters\": [{\"frame_id\": 1056, \"parameters\": [{\"name\": \"GEAR\","
        "     \"expression\": \"B0&7\", \"unit\": \"\"}]}],"
        "  \"pids\": [{\"pid\": \"2101\", \"enabled\": true,"
        "     \"parameters\": [{\"name\": \"OILT\", \"expression\": \"B4-40\", \"unit\": \"C\","
        "        \"min\": \"-40\", \"max\": \"150\"}]}]}]}";
    put_auto_pid(stripped_json);
    put_file("car_data.json", car_legacy);
    autopid_config_t *car_l = load_autopid_config();
    put_file("car_data.json", car_stripped);
    autopid_config_t *car_s = load_autopid_config();
    CHECK(car_l && car_l->pid_count == 6, "5 auto_pid rows + 1 car_data row, got %u", car_l ? (unsigned)car_l->pid_count : 0u);
    if (car_l && car_l->pid_count == 6)
    {
        const pid_data_t *cp = &car_l->pids[5];
        CHECK(cp->pid_type == PID_SPECIFIC && cp->enabled && str_eq(cp->cmd, "2101\r") && cp->mode == 0x21 &&
              cp->parameters_count == 1 && str_eq(cp->parameters[0].name, "OILT") &&
              cp->parameters[0].min == -40.0f && cp->parameters[0].max == 150.0f,
              "car_data row appended after auto_pid's: cmd \"%s\"", show(cp->cmd));
        CHECK(car_l->can_filters_count == 3 && car_l->can_filters[2].frame_id == 1056 &&
              car_l->can_filters[2].is_vehicle_specific && !car_l->can_filters[0].is_vehicle_specific &&
              car_l->can_filters[2].parameters_count == 1 && str_eq(car_l->can_filters[2].parameters[0].name, "GEAR"),
              "car_data filter appended and marked vehicle-specific");
    }
    why[0] = '\0';
    CHECK(same_table(car_l, car_s, true, why, sizeof(why)),
          "car_data retired keys ignored; first difference: %s", why);

    /* Damaged: numeric "pid", "parameters" as an object, as a string, a non-object row. */
    put_file("car_data.json",
             "{\"cars\": [{\"can_filters\": {\"frame_id\": 1},"
             " \"pids\": [{\"pid\": 2101, \"parameters\": {\"name\": \"OBJ\"}},"
             "            {\"pid\": \"2102\", \"parameters\": \"str\"}, 7]}]}");
    autopid_config_t *car_bad = load_autopid_config();
    CHECK(car_bad && car_bad->pid_count == 8 && car_bad->can_filters_count == 2,
          "damaged car_data -> a table (5 + 3 rows, filters object ignored), got %u rows",
          car_bad ? (unsigned)car_bad->pid_count : 0u);
    if (car_bad && car_bad->pid_count == 8)
    {
        bool none_enabled = true;
        for (uint32_t i = 5; i < 8; i++)
            for (uint32_t j = 0; car_bad->pids[i].parameters && j < car_bad->pids[i].parameters_count; j++)
                none_enabled = none_enabled && !car_bad->pids[i].parameters[j].enabled;
        CHECK(car_bad->pids[5].cmd == NULL && none_enabled,
              "numeric pid -> no command; non-array parameters never become an enabled channel");
    }
    remove("car_data.json");

    printf("# P7 deep free (ASan/LSan check the rest at exit)\n");
    autopid_config_t *all[] = { legacy, stripped, def, none, trunc, obj, bad, capped, sess, sess_bad, after,
                                car_l, car_s, car_bad };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        autopid_config_deep_free(all[i]);
    autopid_config_deep_free(NULL);

    remove("auto_pid.json");
    if (chdir("/") != 0 || rmdir(tmpdir) != 0)
        CHECK(false, "temp dir %s left non-empty: the parser wrote a file", tmpdir);
    free(legacy_json);
    free(stripped_json);
    free(default_json);

    if (g_failures)
    {
        printf("FAIL: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("ok: autopid_config %d checks\n", g_checks);
    return 0;
}
