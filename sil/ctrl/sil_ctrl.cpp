// sil_ctrl.cpp - control server commands (JSON). The TCP part lives in sil_ctrl_net.cpp.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "sil_ctrl.h"
#include "sil_ctrl_internal.h"
#include "SilCan.h"
#include "SilOs.h"
#include "sil_port.h"
#include "sil_runtime.h"

namespace {

// ---------------------------------------------------------------------------------------------
// minimal JSON
struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double num = 0.0;
    uint64_t u64 = 0;
    bool isInt = false;
    std::string s;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal *get(const char *key) const {
        for (const auto &kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    uint64_t getU64(const char *key, uint64_t def) const {
        const JVal *v = get(key);
        if (v == nullptr || v->t != Num) return def;
        return v->isInt ? v->u64 : static_cast<uint64_t>(v->num);
    }
    long long getI64(const char *key, long long def) const {
        const JVal *v = get(key);
        if (v == nullptr || v->t != Num) return def;
        return static_cast<long long>(v->num);
    }
    bool getBool(const char *key, bool def) const {
        const JVal *v = get(key);
        if (v == nullptr) return def;
        if (v->t == Bool) return v->b;
        if (v->t == Num) return v->num != 0.0;
        return def;
    }
    std::string getStr(const char *key, const std::string &def) const {
        const JVal *v = get(key);
        return (v != nullptr && v->t == Str) ? v->s : def;
    }
};

class JParser {
public:
    JParser(const char *p, const char *end) : p_(p), end_(end) {}
    bool parse(JVal &out) {
        ws();
        if (!value(out)) return false;
        ws();
        return p_ == end_;
    }

private:
    const char *p_;
    const char *end_;
    void ws() {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) ++p_;
    }
    bool lit(const char *s) {
        size_t n = std::strlen(s);
        if (static_cast<size_t>(end_ - p_) < n || std::strncmp(p_, s, n) != 0) return false;
        p_ += n;
        return true;
    }
    bool str(std::string &out) {
        if (p_ >= end_ || *p_ != '"') return false;
        ++p_;
        while (p_ < end_ && *p_ != '"') {
            char c = *p_++;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (p_ >= end_) return false;
            char e = *p_++;
            switch (e) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (end_ - p_ < 4) return false;
                unsigned cp = 0;
                for (int i = 0; i < 4; ++i) {
                    char h = *p_++;
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= static_cast<unsigned>(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned>(h - 'A' + 10);
                    else return false;
                }
                if (cp < 0x80) out.push_back(static_cast<char>(cp));
                else if (cp < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default: return false;
            }
        }
        if (p_ >= end_) return false;
        ++p_;
        return true;
    }
    bool value(JVal &v) {
        if (p_ >= end_) return false;
        char c = *p_;
        if (c == '{') {
            v.t = JVal::Obj;
            ++p_;
            ws();
            if (p_ < end_ && *p_ == '}') {
                ++p_;
                return true;
            }
            for (;;) {
                ws();
                std::string k;
                if (!str(k)) return false;
                ws();
                if (p_ >= end_ || *p_ != ':') return false;
                ++p_;
                ws();
                JVal child;
                if (!value(child)) return false;
                v.obj.emplace_back(std::move(k), std::move(child));
                ws();
                if (p_ < end_ && *p_ == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < end_ && *p_ == '}') {
                    ++p_;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            v.t = JVal::Arr;
            ++p_;
            ws();
            if (p_ < end_ && *p_ == ']') {
                ++p_;
                return true;
            }
            for (;;) {
                ws();
                JVal child;
                if (!value(child)) return false;
                v.arr.push_back(std::move(child));
                ws();
                if (p_ < end_ && *p_ == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < end_ && *p_ == ']') {
                    ++p_;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            v.t = JVal::Str;
            return str(v.s);
        }
        if (lit("true")) {
            v.t = JVal::Bool;
            v.b = true;
            return true;
        }
        if (lit("false")) {
            v.t = JVal::Bool;
            v.b = false;
            return true;
        }
        if (lit("null")) {
            v.t = JVal::Null;
            return true;
        }
        // number
        const char *start = p_;
        if (p_ < end_ && (*p_ == '-' || *p_ == '+')) ++p_;
        bool isInt = true;
        while (p_ < end_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' || *p_ == 'E' || *p_ == '-' || *p_ == '+')) {
            if (*p_ == '.' || *p_ == 'e' || *p_ == 'E') isInt = false;
            ++p_;
        }
        if (p_ == start) return false;
        std::string tok(start, p_);
        v.t = JVal::Num;
        v.num = std::strtod(tok.c_str(), nullptr);
        v.isInt = isInt && tok[0] != '-';
        if (v.isInt) v.u64 = std::strtoull(tok.c_str(), nullptr, 10);
        return true;
    }
};

std::string jesc(const std::string &s) {
    std::string o = "\"";
    for (char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                o += buf;
            } else {
                o.push_back(c);
            }
        }
    }
    o += "\"";
    return o;
}

std::string u64s(uint64_t v) { return std::to_string(static_cast<unsigned long long>(v)); }

std::string hexstr(const uint8_t *d, unsigned n) {
    static const char *hx = "0123456789abcdef";
    std::string o;
    for (unsigned i = 0; i < n; ++i) {
        o.push_back(hx[d[i] >> 4]);
        o.push_back(hx[d[i] & 15]);
    }
    return o;
}

bool unhex(const std::string &s, std::vector<uint8_t> &out) {
    std::string t;
    for (char c : s)
        if (c != ' ' && c != ':') t.push_back(c);
    if (t.size() % 2 != 0) return false;
    for (size_t i = 0; i < t.size(); i += 2) {
        unsigned v = 0;
        for (int k = 0; k < 2; ++k) {
            char h = t[i + k];
            v <<= 4;
            if (h >= '0' && h <= '9') v |= static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') v |= static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= static_cast<unsigned>(h - 'A' + 10);
            else return false;
        }
        out.push_back(static_cast<uint8_t>(v));
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// frame capture (sniffer on every controller)
struct Captured {
    uint64_t t_ns;
    uint32_t id;
    uint8_t ctrl, dir, flags, len;
    uint8_t data[SILBUS_MAX_DATA];
};
constexpr size_t CAP_SIZE = 8192;
std::mutex g_capMutex;
Captured g_cap[CAP_SIZE];
uint64_t g_capWritten = 0;
uint64_t g_capRead = 0;

void sniff(void *, uint8 ctrl, int isTx, const SilBus_Frame *f) {
    std::lock_guard<std::mutex> l(g_capMutex);
    Captured &c = g_cap[g_capWritten % CAP_SIZE];
    c.t_ns = SilOs_ClockNow();
    c.id = f->id;
    c.ctrl = ctrl;
    c.dir = isTx ? 1u : 0u;
    c.flags = f->flags;
    c.len = f->len;
    std::memcpy(c.data, f->data, f->len);
    g_capWritten++;
}

// ---------------------------------------------------------------------------------------------
std::string ok(const std::string &fields = std::string()) { return fields.empty() ? "{\"ok\":true}" : "{\"ok\":true," + fields + "}"; }
std::string err(const std::string &msg) { return "{\"ok\":false,\"error\":" + jesc(msg) + "}"; }

std::string time_fields() {
    uint64_t limit = SilOs_GetTimeLimit();
    uint64_t now = SilOs_ClockNow();
    std::string o = "\"t_ns\":" + u64s(now);
    o += ",\"mode\":";
    o += (SilOs_ClockMode() == SILOS_TIME_REALTIME) ? "\"realtime\"" : "\"virtual\"";
    o += ",\"limit_ns\":";
    o += (limit == UINT64_MAX) ? "null" : u64s(limit);
    o += ",\"paused\":";
    o += (limit != UINT64_MAX && limit <= now) ? "true" : "false";
    return o;
}

std::string stats_json() {
    std::string o = time_fields();
    o += ",\"errors\":" + u64s(SilOs_ErrorCount());
    o += ",\"tasks\":{";
    for (uint32 i = 0; i < SilOs_Cfg.numTasks; ++i) {
        if (i) o += ",";
        o += jesc(SilOs_Cfg.tasks[i].name) + ":" + u64s(SilOs_TaskActivations(i));
    }
    o += "},\"isrs\":{";
    for (uint32 i = 0; i < SilOs_Cfg.numIsrs; ++i) {
        if (i) o += ",";
        o += jesc(SilOs_Cfg.isrs[i].name) + ":" + u64s(SilOs_IsrRuns(i));
    }
    o += "},\"can\":[";
    for (uint8 c = 0; c < SilCan_NumControllers(); ++c) {
        SilCan_Stats s;
        SilCan_GetStats(c, &s);
        if (c) o += ",";
        o += "{\"name\":" + jesc(SilCan_ControllerName(c)) + ",\"state\":" + std::to_string(s.state) + ",\"tx\":" + std::to_string(s.txFrames) +
             ",\"rx\":" + std::to_string(s.rxFrames) + ",\"rx_dropped\":" + std::to_string(s.rxDropped) + ",\"tx_busy\":" + std::to_string(s.txBusy) +
             ",\"busoff\":" + std::to_string(s.busOff) + ",\"bus\":" + jesc(SilCan_GetBusSpec(c)) + "}";
    }
    o += "]";
    return o;
}

std::string run_until(uint64_t t, uint64_t timeoutMs) {
    if (SilOs_ClockMode() != SILOS_TIME_VIRTUAL) return err("time stepping needs --time=virtual");
    SilOs_SetTimeLimit(t);
    int q = SilOs_WaitQuiescent(t, timeoutMs * 1000000ull);
    return ok(time_fields() + ",\"quiescent\":" + (q ? "true" : "false"));
}

std::string handle(const JVal &req) {
    std::string cmd = req.getStr("cmd", "");
    if (cmd == "ping") return ok("\"t_ns\":" + u64s(SilOs_ClockNow()) + ",\"version\":\"microsarSIL control 1\"");
    if (cmd == "time") return ok(time_fields());
    if (cmd == "run_until") return run_until(req.getU64("t_ns", SilOs_ClockNow()), req.getU64("timeout_ms", 60000));
    if (cmd == "step") return run_until(SilOs_ClockNow() + req.getU64("ns", 1000000), req.getU64("timeout_ms", 60000));
    if (cmd == "pause") return run_until(SilOs_ClockNow(), req.getU64("timeout_ms", 60000));
    if (cmd == "resume") {
        SilOs_SetTimeLimit(UINT64_MAX);
        return ok(time_fields());
    }
    if (cmd == "stop") {
        SilOs_RequestStop(static_cast<int>(req.getI64("code", 0)));
        return ok();
    }
    if (cmd == "stats") return ok(stats_json());
    if (cmd == "can_send") {
        SilBus_Frame f;
        std::memset(&f, 0, sizeof f);
        uint8 ctrl = static_cast<uint8>(req.getU64("ctrl", 0));
        if (ctrl >= SilCan_NumControllers()) return err("no such controller");
        f.id = static_cast<uint32_t>(req.getU64("id", 0)) & 0x1FFFFFFFu;
        bool ext = req.getBool("ext", f.id > 0x7FFu);
        std::vector<uint8_t> data;
        const JVal *d = req.get("data");
        if (d != nullptr && d->t == JVal::Str) {
            if (!unhex(d->s, data)) return err("data: hex string expected");
        } else if (d != nullptr && d->t == JVal::Arr) {
            for (const JVal &b : d->arr) data.push_back(static_cast<uint8_t>(b.num));
        }
        if (data.size() > SILBUS_MAX_DATA) return err("data too long");
        f.len = static_cast<uint8_t>(data.size());
        if (f.len) std::memcpy(f.data, data.data(), f.len);
        f.flags = static_cast<uint8_t>((ext ? SILBUS_FLAG_EXT : 0u) | (req.getBool("fd", f.len > 8u) ? SILBUS_FLAG_FD : 0u) |
                                       (req.getBool("brs", false) ? SILBUS_FLAG_BRS : 0u) | (req.getBool("rtr", false) ? SILBUS_FLAG_RTR : 0u));
        f.t_ns = SilOs_ClockNow();
        SilCan_Receive(ctrl, &f);
        return ok("\"t_ns\":" + u64s(f.t_ns));
    }
    if (cmd == "can_recv") {
        uint64_t max = req.getU64("max", 256);
        std::string o = "\"frames\":[";
        uint64_t dropped = 0;
        {
            std::lock_guard<std::mutex> l(g_capMutex);
            if (g_capWritten > g_capRead + CAP_SIZE) {
                dropped = g_capWritten - CAP_SIZE - g_capRead;
                g_capRead = g_capWritten - CAP_SIZE;
            }
            uint64_t n = 0;
            while (g_capRead < g_capWritten && n < max) {
                const Captured &c = g_cap[g_capRead % CAP_SIZE];
                if (n) o += ",";
                o += "{\"t_ns\":" + u64s(c.t_ns) + ",\"ctrl\":" + std::to_string(c.ctrl) + ",\"dir\":" + (c.dir ? "\"tx\"" : "\"rx\"") +
                     ",\"id\":" + std::to_string(c.id) + ",\"ext\":" + ((c.flags & SILBUS_FLAG_EXT) ? "true" : "false") +
                     ",\"fd\":" + ((c.flags & SILBUS_FLAG_FD) ? "true" : "false") + ",\"data\":\"" + hexstr(c.data, c.len) + "\"}";
                g_capRead++;
                n++;
            }
        }
        o += "],\"dropped\":" + u64s(dropped);
        return ok(o);
    }
    if (cmd == "can_busoff") {
        uint8 ctrl = static_cast<uint8>(req.getU64("ctrl", 0));
        if (ctrl >= SilCan_NumControllers()) return err("no such controller");
        SilCan_InjectBusOff(ctrl);
        return ok();
    }
    if (cmd == "can_bus") {
        long long ctrl = req.getI64("ctrl", -1);
        std::string spec = req.getStr("spec", "null");
        if (ctrl < 0) {
            for (uint8 c = 0; c < SilCan_NumControllers(); ++c) SilCan_SetBus(c, spec.c_str());
        } else if (SilCan_SetBus(static_cast<uint8>(ctrl), spec.c_str()) != 0) {
            return err("no such controller");
        }
        return ok();
    }
    if (cmd == "irq") {
        std::string name = req.getStr("isr", "");
        ISRType id = SilOs_IsrByHandler(name.c_str());
        if (id >= SilOs_NumIsrs()) return err("unknown ISR " + name);
        SilOs_RaiseIrq(id);
        return ok("\"isr\":" + std::to_string(static_cast<unsigned>(id)));
    }
    if (cmd == "user") {
        SilOs_UserEvent(static_cast<uint32>(req.getU64("id", 0)), static_cast<uint32>(req.getU64("aux", 0)));
        return ok();
    }
    if (cmd == "write_traces") {
        SilRt_WriteTraces();
        return ok();
    }
    if (cmd == "help")
        return ok("\"commands\":[\"ping\",\"time\",\"run_until\",\"step\",\"pause\",\"resume\",\"stop\",\"stats\",\"can_send\",\"can_recv\","
                  "\"can_busoff\",\"can_bus\",\"irq\",\"user\",\"write_traces\"]");
    return err("unknown command " + cmd);
}

} // namespace

std::string silctrl_handle_line(const std::string &line) {
    JVal req;
    JParser parser(line.data(), line.data() + line.size());
    std::string reply = (parser.parse(req) && req.t == JVal::Obj) ? handle(req) : err("invalid JSON");
    const JVal *id = req.get("id");
    if (id != nullptr && id->t == JVal::Num) reply.insert(reply.size() - 1, ",\"id\":" + u64s(id->u64));
    return reply;
}

void silctrl_on_start() { SilCan_SetSniffer(sniff, nullptr); }
