#include "blocks.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <set>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace splash {

namespace {

using json = nlohmann::json;
using ojson = nlohmann::ordered_json;

using K = BlockKind;
using F = BlockField;

const std::vector<BlockSpec> kCatalogue = {
    // Hats, in the order their functions come out.
    {"on_create", K::Hat, "when created", 0, 0, F::None, "Events",
     "Runs the blocks under it once, after the scene has loaded, for this object."},
    {"on_update", K::Hat, "every frame", 0, 0, F::None, "Events",
     "Runs the blocks under it once every frame, for this object."},
    {"on_touch_player", K::Hat, "when the player touches me", 0, 0, F::None, "Events",
     "Runs the blocks under it when the player collides with this object."},
    {"on_interact", K::Hat, "when the player interacts", 0, 0, F::None, "Events",
     "Runs the blocks under it when the player interacts with this object."},
    {"on_button_press", K::Hat, "when %f is pressed", 0, 0, F::Button, "Events",
     "Runs the blocks under it when the chosen button is pressed."},
    {"on_button_release", K::Hat, "when %f is released", 0, 0, F::Button, "Events",
     "Runs the blocks under it when the chosen button is let go."},
    {"on_enable", K::Hat, "when enabled", 0, 0, F::None, "Events",
     "Runs the blocks under it when this object is switched on."},
    {"on_disable", K::Hat, "when disabled", 0, 0, F::None, "Events",
     "Runs the blocks under it when this object is switched off."},
    {"on_scene_start", K::Hat, "when the scene starts", 0, 0, F::None, "Events",
     "Runs the blocks under it once, after every object in the scene has been created."},
    {"on_trigger_enter", K::Hat, "when the player enters the trigger", 0, 0, F::None, "Events",
     "Runs the blocks under it when the player enters this trigger's box."},
    {"on_trigger_exit", K::Hat, "when the player leaves the trigger", 0, 0, F::None, "Events",
     "Runs the blocks under it when the player leaves this trigger's box."},
    // Statements and C blocks.
    {"set_var", K::Statement, "set %f to %1", 1, 0, F::Variable, "Variables",
     "Sets the variable to the value."},
    {"change_var", K::Statement, "change %f by %1", 1, 0, F::Variable, "Variables",
     "Adds the value to the variable; a negative number takes it away."},
    {"wait", K::Statement, "wait %1 seconds", 1, 0, F::None, "Control",
     "Pauses this event's blocks for that many seconds while the game runs on; the event is ignored until they finish, and a hidden object's blocks stay paused."},
    {"if", K::CBlock, "if %1 then", 1, 1, F::None, "Control",
     "Runs the blocks inside only when the condition is true."},
    {"if_else", K::CBlock, "if %1 then|else", 1, 2, F::None, "Control",
     "Runs the first blocks inside when the condition is true, the second when it is false."},
    {"repeat", K::CBlock, "repeat %1 times", 1, 1, F::None, "Control",
     "Runs the blocks inside the given number of times."},
    {"while", K::CBlock, "while %1", 1, 1, F::None, "Control",
     "Runs the blocks inside again and again for as long as the condition is true, checking it before each time."},
    {"repeat_until", K::CBlock, "repeat until %1", 1, 1, F::None, "Control",
     "Runs the blocks inside again and again until the condition is true, checking it before each time."},
    {"wait_until", K::Statement, "wait until %1", 1, 0, F::None, "Control",
     "Pauses this event's blocks until the condition is true, checking it once a frame; the event is ignored until they finish."},
    {"stop", K::Statement, "stop this script", 0, 0, F::None, "Control",
     "Ends this event here: the blocks after it in the event do not run this time."},
    {"show", K::Statement, "show %f", 0, 0, F::Object, "Objects",
     "Switches the object on (makes it active)."},
    {"hide", K::Statement, "hide %f", 0, 0, F::Object, "Objects",
     "Switches the object off (makes it inactive)."},
    {"move_by", K::Statement, "move %f by %1 %2 %3", 3, 0, F::Object, "Objects",
     "Moves the object by the x, y and z amounts from where it is now, in game units (scene metres divided by the scene's GTE scaling)."},
    {"go_to", K::Statement, "move %f to %1 %2 %3", 3, 0, F::Object, "Objects",
     "Puts the object at the x, y and z position, in game units (scene metres divided by the scene's GTE scaling)."},
    {"play_sound", K::Statement, "play sound %f", 0, 0, F::Sound, "Sound and scenes",
     "Plays the chosen sound clip."},
    {"play_cutscene", K::Statement, "play cutscene %f", 0, 0, F::Cutscene, "Sound and scenes",
     "Plays the chosen cutscene."},
    {"load_scene", K::Statement, "load scene %1", 1, 0, F::None, "Sound and scenes",
     "Loads the scene with that number; scene 0 is the one the game starts in."},
    {"log", K::Statement, "log %1", 1, 0, F::None, "Debug",
     "Writes the value to the debug log."},
    // Reporters.
    {"number", K::Reporter, "%f", 0, 0, F::Number, "Operators",
     "A number: whole numbers, or decimals from -524288 to 524287 with up to 9 places."},
    {"text", K::Reporter, "%f", 0, 0, F::Text, "Operators",
     "A piece of text, exactly as typed."},
    {"bool", K::Boolean, "%f", 0, 0, F::Bool, "Operators",
     "True or false."},
    {"var", K::Reporter, "%f", 0, 0, F::Variable, "Variables",
     "The variable's value."},
    {"add", K::Reporter, "%1 + %2", 2, 0, F::None, "Operators",
     "The first number plus the second."},
    {"sub", K::Reporter, "%1 - %2", 2, 0, F::None, "Operators",
     "The first number minus the second."},
    {"mul", K::Reporter, "%1 * %2", 2, 0, F::None, "Operators",
     "The first number times the second."},
    {"div", K::Reporter, "%1 / %2", 2, 0, F::None, "Operators",
     "The first number divided by the second."},
    {"mod", K::Reporter, "%1 mod %2", 2, 0, F::None, "Operators",
     "The remainder of the first number divided by the second."},
    {"random", K::Reporter, "pick random %1 to %2", 2, 0, F::None, "Operators",
     "A random whole number from the first number to the second, both included."},
    {"join", K::Reporter, "join %1 %2", 2, 0, F::None, "Operators",
     "The two values joined into one piece of text."},
    {"eq", K::Boolean, "%1 = %2", 2, 0, F::None, "Operators",
     "True when the two values are equal."},
    {"lt", K::Boolean, "%1 < %2", 2, 0, F::None, "Operators",
     "True when the first value is less than the second."},
    {"gt", K::Boolean, "%1 > %2", 2, 0, F::None, "Operators",
     "True when the first value is greater than the second."},
    {"and", K::Boolean, "%1 and %2", 2, 0, F::None, "Operators",
     "True when both conditions are true."},
    {"or", K::Boolean, "%1 or %2", 2, 0, F::None, "Operators",
     "True when either condition is true."},
    {"not", K::Boolean, "not %1", 1, 0, F::None, "Operators",
     "True when the condition is false."},
    {"button_held", K::Boolean, "%f held?", 0, 0, F::Button, "Input",
     "True while player 1 holds the chosen button down."},
    {"frame_count", K::Reporter, "frame count", 0, 0, F::None, "Debug",
     "How many frames the game has counted so far."},
    {"position_x", K::Reporter, "x position of %f", 0, 0, F::Object, "Objects",
     "The object's x position, in game units."},
    {"position_y", K::Reporter, "y position of %f", 0, 0, F::Object, "Objects",
     "The object's y position, in game units."},
    {"position_z", K::Reporter, "z position of %f", 0, 0, F::Object, "Objects",
     "The object's z position, in game units."},
};

const std::vector<std::string_view> kCategories = {"Events", "Control", "Objects", "Sound and scenes",
                                                   "Variables", "Operators", "Input", "Debug"};

const std::vector<std::string_view> kButtons = {"CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "L1", "R1",
                                                "L2",    "R2",     "START",  "SELECT",   "UP", "DOWN",
                                                "LEFT",  "RIGHT",  "L3",     "R3"};

struct Hat {
    std::string_view op, name, params;
    bool self;
};
const Hat kHats[] = {
    {"on_create", "onCreate", "self", true},
    {"on_update", "onUpdate", "self, dt", true},
    {"on_touch_player", "onCollideWithPlayer", "self", true},
    {"on_interact", "onInteract", "self", true},
    {"on_button_press", "onButtonPress", "self, button", true},
    {"on_button_release", "onButtonRelease", "self, button", true},
    {"on_enable", "onEnable", "self", true},
    {"on_disable", "onDisable", "self", true},
    {"on_scene_start", "onSceneCreationEnd", "", false},
    {"on_trigger_enter", "onTriggerEnter", "index", false},
    {"on_trigger_exit", "onTriggerExit", "index", false},
};

const std::set<std::string, std::less<>> kReserved = {
    // Lua 5.2 keywords
    "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in", "local", "nil",
    "not", "or", "repeat", "return", "then", "true", "until", "while",
    // event parameters
    "self", "dt", "index", "button",
    // psxsplash API tables
    "Actor", "Agent", "Animation", "Audio", "Camera", "Controls", "Convert", "Cutscene", "Debug", "Entity",
    "FixedPoint", "Input", "Interact", "Light", "MemCard", "Net", "Persist", "Player", "PSXMath", "Random", "Scene",
    "SkinnedAnim", "Sprite", "Tile", "Timer", "UI", "Vec3",
    // event functions: a local of that name would capture the function
    "onCreate", "onUpdate", "onCollideWithPlayer", "onInteract", "onButtonPress", "onButtonRelease", "onEnable",
    "onDisable", "onSceneCreationStart", "onSceneCreationEnd", "onTriggerEnter", "onTriggerExit",
    // helpers the generated Lua defines for wait blocks
    "tasks", "wait", "startTask", "resumeTask"};

[[noreturn]] void fail(const std::string& path, const std::string& what) { throw std::runtime_error(path + ": " + what); }

// ---- JSON ---------------------------------------------------------------

void onlyKeys(const json& j, std::initializer_list<const char*> keys, const std::string& path, const std::string& at) {
    for (auto it = j.begin(); it != j.end(); ++it)
        if (std::none_of(keys.begin(), keys.end(), [&](const char* k) { return it.key() == k; }))
            fail(path, at + ": unknown key \"" + it.key() + "\"");
}

const json& arrayAt(const json& j, const char* key, const std::string& path, const std::string& at) {
    const json& a = j.at(key);
    if (!a.is_array()) fail(path, at + "." + key + ": not an array");
    return a;
}

std::string stringAt(const json& j, const char* key, const std::string& path, const std::string& at) {
    if (!j.contains(key)) fail(path, at + ": missing \"" + key + "\"");
    if (!j[key].is_string()) fail(path, at + "." + key + ": not a string");
    return j[key].get<std::string>();
}

Block readBlock(const json& j, const std::string& path, const std::string& at) {
    if (!j.is_object()) fail(path, at + ": not an object");
    onlyKeys(j, {"op", "field", "args", "bodies"}, path, at);
    Block b;
    if (j.contains("op")) b.op = stringAt(j, "op", path, at);
    if (j.contains("field")) b.field = stringAt(j, "field", path, at);
    if (j.contains("args")) {
        const json& a = arrayAt(j, "args", path, at);
        for (size_t i = 0; i < a.size(); i++) b.args.push_back(readBlock(a[i], path, at + ".args[" + std::to_string(i) + "]"));
    }
    if (j.contains("bodies")) {
        const json& bs = arrayAt(j, "bodies", path, at);
        for (size_t i = 0; i < bs.size(); i++) {
            std::string bat = at + ".bodies[" + std::to_string(i) + "]";
            if (!bs[i].is_array()) fail(path, bat + ": not an array");
            std::vector<Block>& body = b.bodies.emplace_back();
            for (size_t k = 0; k < bs[i].size(); k++)
                body.push_back(readBlock(bs[i][k], path, bat + "[" + std::to_string(k) + "]"));
        }
    }
    return b;
}

float coord(const json& j, const char* key, const std::string& path, const std::string& at) {
    if (!j.contains(key)) return 0;
    if (!j[key].is_number()) fail(path, at + "." + key + ": not a number");
    return j[key].get<float>();
}

ojson coordJson(float v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15f) return ojson(int64_t(v));
    return ojson(double(v));
}

ojson blockJson(const Block& b) {
    ojson o = ojson::object();
    o["op"] = b.op;
    if (!b.field.empty()) o["field"] = b.field;
    if (!b.args.empty()) {
        ojson a = ojson::array();
        for (const Block& x : b.args) a.push_back(blockJson(x));
        o["args"] = std::move(a);
    }
    if (!b.bodies.empty()) {
        ojson bs = ojson::array();
        for (const std::vector<Block>& body : b.bodies) {
            ojson a = ojson::array();
            for (const Block& x : body) a.push_back(blockJson(x));
            bs.push_back(std::move(a));
        }
        o["bodies"] = std::move(bs);
    }
    return o;
}

// ---- Lua ----------------------------------------------------------------

// Lua 5.2 binary precedence, low to high; atoms are names, calls, literals.
enum Prec { Or = 1, And, Cmp, Concat, Add, Mul, Unary, Atom };

struct Expr {
    std::string s;
    int prec = Atom;
};

std::string luaString(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '"': o += "\\\""; break;
            case '\n': o += "\\n"; break;
            case '\t': o += "\\t"; break;
            case '\r': o += "\\r"; break;
            default:
                if (c < 0x20 || c == 0x7f) o += "\\" + std::to_string(int(c));
                else o += char(c);
        }
    }
    return o + "\"";
}

bool allDigits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// -?digits or -?digits.digits. Returns false when text is not a number;
// err is set when it is one Lua cannot hold.
bool numberLiteral(const std::string& text, Expr& out, std::string& err) {
    std::string_view s = text;
    bool neg = !s.empty() && s[0] == '-';
    if (neg) s.remove_prefix(1);
    size_t dot = s.find('.');
    std::string_view ip = s.substr(0, dot), fp = dot == std::string_view::npos ? std::string_view() : s.substr(dot + 1);
    if (!allDigits(ip) || (dot != std::string_view::npos && !allDigits(fp))) return false;
    ip.remove_prefix(std::min(ip.find_first_not_of('0'), ip.size()));
    if (ip.size() > 10) {
        err = "number " + text + " is out of range";
        return true;
    }
    int64_t whole = ip.empty() ? 0 : std::stoll(std::string(ip));
    int64_t num = whole, den = 1;
    if (!fp.empty()) {
        if (fp.size() > 9) {
            err = "number " + text + " has more than 9 decimal places";
            return true;
        }
        if (whole > 524288) {
            err = "number " + text + " is out of range (decimals go from -524288 to 524287)";
            return true;
        }
        for (char c : fp) num = num * 10 + (c - '0'), den *= 10;
        int64_t g = std::gcd(num, den);
        if (g > 1) num /= g, den /= g;
    }
    if (neg) num = -num;
    if (den == 1) {
        if (num < -2147483647 || num > 2147483647) {
            err = "number " + text + " is out of range";
            return true;
        }
        out = {std::to_string(num), num < 0 ? int(Unary) : int(Atom)};
        return true;
    }
    if (num < -524288 || num > 524287) {
        err = "number " + text + " is out of range (decimals go from -524288 to 524287)";
        return true;
    }
    out = {"FixedPoint.new(" + std::to_string(num) + ") / " + std::to_string(den), Mul};
    return true;
}

Expr literal(const std::string& value, std::string& err) {
    Expr e;
    if (numberLiteral(value, e, err)) return e;
    if (value == "true" || value == "false") return {value, Atom};
    return {luaString(value), Atom};
}

std::string paren(const Expr& e, bool need) { return need ? "(" + e.s + ")" : e.s; }

Expr binary(const Expr& l, const char* op, int prec, const Expr& r) {
    bool right = prec == Concat;  // the one right-associative binary operator here
    return {paren(l, right ? l.prec <= prec : l.prec < prec) + " " + op + " " +
                paren(r, right ? r.prec < prec : r.prec <= prec),
            prec};
}

std::string pad(int depth) { return std::string(size_t(depth) * 4, ' '); }

struct Gen {
    std::set<std::string, std::less<>> vars;
    std::vector<std::string> errors;
    std::vector<std::string> loopVars;
    const Hat* hat = nullptr;
    std::string stopAt;  // where the current stack's first stop is
    bool usesWait = false;

    Expr nil() { return {"nil", Atom}; }
    void error(const std::string& at, const std::string& what) { errors.push_back(at + ": " + what); }

    static std::string where(const std::string& prefix, const char* what, size_t n, const Block& b) {
        return prefix + what + std::to_string(n) + " (" + (b.op.empty() ? "empty" : b.op) + ")";
    }

    // Checks the block's shape; null when it is unknown or the wrong kind.
    const BlockSpec* spec(const Block& b, const std::string& at, bool input) {
        if (b.op.empty()) {
            error(at, "empty block");
            return nullptr;
        }
        const BlockSpec* s = findBlockSpec(b.op);
        if (!s) {
            error(at, "unknown block");
            return nullptr;
        }
        bool reporter = s->kind == K::Reporter || s->kind == K::Boolean;
        if (s->kind == K::Hat) {
            error(at, "a hat block can only start a stack");
            return nullptr;
        }
        if (input && !reporter) {
            error(at, "a statement block cannot be an input");
            return nullptr;
        }
        if (!input && reporter) {
            error(at, "a reporter block cannot be a statement");
            return nullptr;
        }
        if (b.args.size() > size_t(s->args)) error(at, "too many inputs");
        if (b.bodies.size() > size_t(s->bodies)) error(at, "too many bodies");
        return s;
    }

    Expr input(const Block& b, size_t i, const std::string& at) {
        if (i >= b.args.size() || b.args[i].op.empty()) {
            error(at, "missing input " + std::to_string(i + 1));
            return nil();
        }
        return expr(b.args[i], where(at + ", ", "input ", i + 1, b.args[i]));
    }

    Expr object(const Block& b, const std::string& at) {
        if (b.field == "self") {
            if (!hat->self) error(at, "self is not available in " + std::string(hat->op));
            return {"self", Atom};
        }
        if (b.field.empty()) {
            error(at, "no object chosen");
            return nil();
        }
        return {"Entity.Find(" + luaString(b.field) + ")", Atom};
    }

    std::string button(const Block& b, const std::string& at) {
        if (b.field.empty()) error(at, "no button chosen");
        else if (std::find(kButtons.begin(), kButtons.end(), b.field) == kButtons.end())
            error(at, "unknown button \"" + b.field + "\"");
        else return "Input." + b.field;
        return "nil";
    }

    std::string variable(const Block& b, const std::string& at) {
        if (b.field.empty()) error(at, "no variable chosen");
        else if (!vars.count(b.field)) error(at, "undeclared variable \"" + b.field + "\"");
        return b.field.empty() ? "nil" : b.field;
    }

    Expr expr(const Block& b, const std::string& at) {
        const BlockSpec* s = spec(b, at, true);
        if (!s) return nil();
        const std::string& op = b.op;
        auto bin = [&](const char* lua, int prec) { return binary(input(b, 0, at), lua, prec, input(b, 1, at)); };
        if (op == "number") {
            Expr e;
            std::string err;
            if (!numberLiteral(b.field, e, err)) error(at, b.field.empty() ? "no number" : "\"" + b.field + "\" is not a number");
            else if (!err.empty()) error(at, err);
            else return e;
            return nil();
        }
        if (op == "text") return {luaString(b.field), Atom};
        if (op == "bool") {
            if (b.field != "true" && b.field != "false") {
                error(at, "\"" + b.field + "\" is not true or false");
                return nil();
            }
            return {b.field, Atom};
        }
        if (op == "var") return {variable(b, at), Atom};
        if (op == "add") return bin("+", Add);
        if (op == "sub") return bin("-", Add);
        if (op == "mul") return bin("*", Mul);
        if (op == "div") return bin("/", Mul);
        if (op == "mod") return bin("%", Mul);
        if (op == "join") return bin("..", Concat);
        if (op == "eq") return bin("==", Cmp);
        if (op == "lt") return bin("<", Cmp);
        if (op == "gt") return bin(">", Cmp);
        if (op == "and") return bin("and", And);
        if (op == "or") return bin("or", Or);
        if (op == "not") {
            Expr e = input(b, 0, at);
            return {"not " + paren(e, e.prec < Unary), Unary};
        }
        if (op == "random") {
            Expr a = input(b, 0, at), c = input(b, 1, at);
            return {"Random.Range(" + a.s + ", " + c.s + ")", Atom};
        }
        if (op == "button_held") return {"Input.IsHeldPlayer1(" + button(b, at) + ")", Atom};
        if (op == "frame_count") return {"Timer.GetFrameCount()", Atom};
        if (op == "position_x" || op == "position_y" || op == "position_z")
            return {"Entity.GetPosition(" + object(b, at).s + ")." + op.substr(9), Atom};
        error(at, "unknown block");
        return nil();
    }

    void line(std::string& out, int depth, const std::string& text) { out += pad(depth) + text + "\n"; }

    std::string loopVar(const std::string& at) {
        for (char c = 'i'; c <= 'z'; c++) {
            std::string v(1, c);
            if (!vars.count(v) && std::find(loopVars.begin(), loopVars.end(), v) == loopVars.end()) return v;
        }
        error(at, "repeat blocks nested too deep");
        return "_";
    }

    // Blocks numbered from `first` under `prefix` ("stack 2, " or "..., body 1, ").
    void statements(const std::vector<Block>& list, int depth, const std::string& prefix, size_t first, std::string& out) {
        for (size_t i = 0; i < list.size(); i++) {
            const Block& b = list[i];
            std::string at = where(prefix, "block ", first + i, b);
            const BlockSpec* s = spec(b, at, false);
            if (!s) continue;
            const std::string& op = b.op;
            auto body = [&](size_t n, int d) {
                if (n < b.bodies.size()) statements(b.bodies[n], d, at + ", body " + std::to_string(n + 1) + ", ", 1, out);
            };
            if (op == "set_var") {
                std::string v = variable(b, at);
                line(out, depth, v + " = " + input(b, 0, at).s);
            } else if (op == "change_var") {
                std::string v = variable(b, at);
                const Block* a = b.args.empty() ? nullptr : &b.args[0];
                if (a && a->op == "number" && a->field.size() > 1 && a->field[0] == '-') {
                    Block pos = *a;
                    pos.field.erase(0, 1);
                    Expr e = expr(pos, where(at + ", ", "input ", 1, *a));
                    line(out, depth, v + " = " + binary({v, Atom}, "-", Add, e).s);
                } else {
                    line(out, depth, v + " = " + binary({v, Atom}, "+", Add, input(b, 0, at)).s);
                }
            } else if (op == "if" || op == "if_else") {
                line(out, depth, "if " + input(b, 0, at).s + " then");
                body(0, depth + 1);
                if (op == "if_else") {
                    line(out, depth, "else");
                    body(1, depth + 1);
                }
                line(out, depth, "end");
            } else if (op == "repeat") {
                std::string v = loopVar(at);
                line(out, depth, "for " + v + " = 1, " + input(b, 0, at).s + " do");
                loopVars.push_back(v);
                body(0, depth + 1);
                loopVars.pop_back();
                line(out, depth, "end");
            } else if (op == "while") {
                line(out, depth, "while " + input(b, 0, at).s + " do");
                body(0, depth + 1);
                line(out, depth, "end");
            } else if (op == "repeat_until") {
                Expr c = input(b, 0, at);
                line(out, depth, "while not " + paren(c, c.prec < Unary) + " do");
                body(0, depth + 1);
                line(out, depth, "end");
            } else if (op == "wait" || op == "wait_until") {
                if (!hat->self) error(at, "wait only works in object scripts: the scene and trigger events get no frames");
                if (op == "wait") {
                    usesWait = true;
                    line(out, depth, "wait(" + input(b, 0, at).s + ")");
                } else {
                    Expr c = input(b, 0, at);
                    line(out, depth, "while not " + paren(c, c.prec < Unary) + " do");
                    line(out, depth + 1, "coroutine.yield()");
                    line(out, depth, "end");
                }
            } else if (op == "stop") {
                if (i + 1 < list.size()) error(at, "nothing can follow stop");
                if (stopAt.empty()) stopAt = at;
                line(out, depth, "return");
            } else if (op == "show" || op == "hide") {
                line(out, depth, "Entity.SetActive(" + object(b, at).s + (op == "show" ? ", true)" : ", false)"));
            } else if (op == "move_by" || op == "go_to") {
                std::string o = object(b, at).s;
                Expr x = input(b, 0, at), y = input(b, 1, at), z = input(b, 2, at);
                std::string v = "Vec3.new(" + x.s + ", " + y.s + ", " + z.s + ")";
                if (op == "move_by") v = "Vec3.add(Entity.GetPosition(" + o + "), " + v + ")";
                line(out, depth, "Entity.SetPosition(" + o + ", " + v + ")");
            } else if (op == "play_sound" || op == "play_cutscene") {
                bool sound = op == "play_sound";
                if (b.field.empty()) error(at, sound ? "no sound chosen" : "no cutscene chosen");
                line(out, depth, (sound ? "Audio.Play(" : "Cutscene.Play(") + luaString(b.field) + ")");
            } else if (op == "load_scene") {
                line(out, depth, "Scene.Load(" + input(b, 0, at).s + ")");
            } else if (op == "log") {
                line(out, depth, "Debug.Log(" + input(b, 0, at).s + ")");
            } else {
                error(at, "unknown block");
            }
        }
    }
};

struct Part {
    size_t stack;
    std::string code, stopAt;
    bool task = false;  // the stack waits, so it runs as a task
};

// Bodies of stacks sharing a hat, one after the other with a blank line
// between. A stop returns from the whole function, so it may only sit in
// the last of them.
std::string mergeParts(const std::vector<Part>& parts, Gen& g) {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (!parts[i].stopAt.empty() && i + 1 < parts.size())
            g.error(parts[i].stopAt, "stop would also skip stack " + std::to_string(parts[i + 1].stack) +
                                         ", which has the same hat");
        if (parts[i].code.empty()) continue;
        if (!out.empty()) out += "\n";
        out += parts[i].code;
    }
    return out;
}

}  // namespace

std::string blockNameError(std::string_view name) {
    if (name.empty()) return "no name";
    bool ident = !(name[0] >= '0' && name[0] <= '9') && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
    if (!ident) return "not a name (letters, digits and _, not starting with a digit)";
    if (kReserved.count(name)) return "\"" + std::string(name) + "\" is reserved";
    return {};
}

const std::vector<BlockSpec>& blockCatalogue() { return kCatalogue; }

const BlockSpec* findBlockSpec(std::string_view op) {
    for (const BlockSpec& s : kCatalogue)
        if (s.op == op) return &s;
    return nullptr;
}

std::string_view hatCallback(std::string_view op) {
    for (const Hat& hat : kHats)
        if (hat.op == op) return hat.name;
    return {};
}

const std::vector<std::string_view>& blockButtons() { return kButtons; }

const std::vector<std::string_view>& blockCategories() { return kCategories; }

BlockScript parseBlocks(const std::string& text, const std::string& path) {
    json j;
    try {
        j = json::parse(text);
    } catch (const json::exception& e) {
        fail(path, e.what());
    }
    if (!j.is_object()) fail(path, "not a block script");
    if (!j.contains("format") || j["format"] != "splashedit-blocks") fail(path, "not a splashedit block script");
    if (!j.contains("version") || !j["version"].is_number_integer()) fail(path, "missing or invalid \"version\"");
    int64_t v = j["version"].get<int64_t>();
    if (v < 1 || v > 1) fail(path, "format version " + std::to_string(v) + ", this build reads up to 1");
    const std::string top = "file";
    onlyKeys(j, {"format", "version", "variables", "stacks"}, path, top);
    BlockScript s;
    if (j.contains("variables")) {
        const json& vs = arrayAt(j, "variables", path, top);
        for (size_t i = 0; i < vs.size(); i++) {
            std::string at = "variables[" + std::to_string(i) + "]";
            if (!vs[i].is_object()) fail(path, at + ": not an object");
            onlyKeys(vs[i], {"name", "value"}, path, at);
            s.variables.push_back({stringAt(vs[i], "name", path, at), stringAt(vs[i], "value", path, at)});
        }
    }
    if (j.contains("stacks")) {
        const json& ss = arrayAt(j, "stacks", path, top);
        for (size_t i = 0; i < ss.size(); i++) {
            std::string at = "stacks[" + std::to_string(i) + "]";
            if (!ss[i].is_object()) fail(path, at + ": not an object");
            onlyKeys(ss[i], {"x", "y", "blocks"}, path, at);
            BlockStack st;
            st.x = coord(ss[i], "x", path, at);
            st.y = coord(ss[i], "y", path, at);
            if (ss[i].contains("blocks")) {
                const json& bs = arrayAt(ss[i], "blocks", path, at);
                for (size_t k = 0; k < bs.size(); k++)
                    st.blocks.push_back(readBlock(bs[k], path, at + ".blocks[" + std::to_string(k) + "]"));
            }
            s.stacks.push_back(std::move(st));
        }
    }
    return s;
}

std::string saveBlocks(const BlockScript& s) {
    ojson j = ojson::object();
    j["format"] = "splashedit-blocks";
    j["version"] = 1;
    ojson vs = ojson::array();
    for (const BlockVariable& v : s.variables) {
        ojson o = ojson::object();
        o["name"] = v.name;
        o["value"] = v.value;
        vs.push_back(std::move(o));
    }
    j["variables"] = std::move(vs);
    ojson ss = ojson::array();
    for (const BlockStack& st : s.stacks) {
        ojson o = ojson::object();
        o["x"] = coordJson(st.x);
        o["y"] = coordJson(st.y);
        ojson bs = ojson::array();
        for (const Block& b : st.blocks) bs.push_back(blockJson(b));
        o["blocks"] = std::move(bs);
        ss.push_back(std::move(o));
    }
    j["stacks"] = std::move(ss);
    return j.dump(2) + "\n";
}

LuaGen blocksToLua(const BlockScript& script) {
    Gen g;
    std::string vars;
    for (size_t i = 0; i < script.variables.size(); i++) {
        const BlockVariable& v = script.variables[i];
        std::string at = "variable " + std::to_string(i + 1) + " (" + v.name + ")";
        std::string bad = blockNameError(v.name);
        if (!bad.empty()) g.error(at, bad);
        else if (g.vars.count(v.name)) g.error(at, "declared twice");
        std::string err;
        Expr e = literal(v.value, err);
        if (!err.empty()) g.error(at, err);
        g.vars.insert(v.name);
        vars += "local " + v.name + " = " + e.s + "\n";
    }

    // A stack whose blocks wait runs them as a task: a coroutine onUpdate
    // resumes every frame, so it runs alongside the hat's other stacks.
    auto waits = [](const std::vector<Block>& list, size_t from, auto& self) -> bool {
        for (size_t i = from; i < list.size(); i++) {
            if (list[i].op == "wait" || list[i].op == "wait_until") return true;
            for (const auto& body : list[i].bodies)
                if (self(body, 0, self)) return true;
        }
        return false;
    };
    std::vector<std::string> tasks;

    struct Function {
        std::string name, params, body;
        bool onlyForTasks = false;  // onUpdate with no stack of its own
    };
    std::vector<Function> functions;
    // psxsplash never calls onDestroy (no object is destroyed at runtime), so
    // the hat left the catalogue; a saved stack under it would otherwise be a
    // loose stack and vanish without a word.
    for (size_t si = 0; si < script.stacks.size(); si++) {
        const std::vector<Block>& blocks = script.stacks[si].blocks;
        if (!blocks.empty() && blocks[0].op == "on_destroy")
            g.error(Gen::where("stack " + std::to_string(si + 1) + ", ", "block ", 1, blocks[0]),
                    "\"when destroyed\" never runs, psxsplash does not destroy objects; move these blocks under "
                    "another event");
    }
    for (const Hat& hat : kHats) {
        g.hat = &hat;
        bool buttons = hat.op == "on_button_press" || hat.op == "on_button_release";
        std::vector<std::vector<Part>> arms(buttons ? kButtons.size() : 1);
        bool any = false;
        for (size_t si = 0; si < script.stacks.size(); si++) {
            const std::vector<Block>& blocks = script.stacks[si].blocks;
            if (blocks.empty() || blocks[0].op != hat.op) continue;
            std::string prefix = "stack " + std::to_string(si + 1) + ", ";
            size_t arm = 0;
            if (buttons) {
                std::string at = Gen::where(prefix, "block ", 1, blocks[0]);
                auto it = std::find(kButtons.begin(), kButtons.end(), blocks[0].field);
                if (it == kButtons.end()) {
                    g.button(blocks[0], at);
                    continue;
                }
                arm = size_t(it - kButtons.begin());
            }
            Part p{si + 1, {}, {}, false};
            g.stopAt.clear();
            p.task = waits(blocks, 1, waits);
            int depth = (buttons ? 2 : 1) + (p.task ? 1 : 0);
            g.statements(std::vector<Block>(blocks.begin() + 1, blocks.end()), depth, prefix, 2, p.code);
            p.stopAt = g.stopAt;
            arms[arm].push_back(std::move(p));
            any = true;
        }
        if (!any) {
            if (hat.op == "on_update") functions.push_back({std::string(hat.name), std::string(hat.params), {}, true});
            continue;
        }
        // The arm's stacks in order, each one that waits wrapped in its task;
        // a stop in a task ends only that task.
        auto armBody = [&](size_t a, int depth) {
            size_t count = 0, n = 0;
            for (const Part& p : arms[a]) count += p.task;
            for (Part& p : arms[a]) {
                if (!p.task) continue;
                std::string name(hat.name);
                if (buttons) name += "_" + std::string(kButtons[a]);
                if (count > 1) name += "_" + std::to_string(++n);
                tasks.push_back(name);
                p.code = pad(depth) + "startTask(\"" + name + "\", function()\n" + p.code + pad(depth) + "end)\n";
                p.stopAt.clear();
            }
            return mergeParts(arms[a], g);
        };
        std::string f;
        if (buttons) {
            bool first = true;
            for (size_t b = 0; b < arms.size(); b++) {
                if (arms[b].empty()) continue;
                f += pad(1) + (first ? "if" : "elseif") + " button == Input." + std::string(kButtons[b]) + " then\n";
                f += armBody(b, 2);
                first = false;
            }
            f += pad(1) + "end\n";
        } else {
            f += armBody(0, 1);
        }
        functions.push_back({std::string(hat.name), std::string(hat.params), f, false});
    }

    std::string helpers;
    if (!tasks.empty()) {
        helpers =
            "-- Blocks that wait run as tasks (coroutines); onUpdate resumes them every frame.\n"
            "local tasks = {}\n"
            "\n"
            "local function resumeTask(name, ...)\n"
            "    local ok, err = coroutine.resume(tasks[name], ...)\n"
            "    if coroutine.status(tasks[name]) == \"dead\" then\n"
            "        tasks[name] = nil\n"
            "    end\n"
            "    if not ok then\n"
            "        error(err, 0)\n"
            "    end\n"
            "end\n"
            "\n"
            "-- Starts fn as the task called name, unless that task is still waiting.\n"
            "local function startTask(name, fn)\n"
            "    if tasks[name] == nil then\n"
            "        tasks[name] = coroutine.create(fn)\n"
            "        resumeTask(name)\n"
            "    end\n"
            "end\n";
        if (g.usesWait)
            helpers +=
                "\n"
                "-- dt counts 4096 per 30fps frame, so a second is 30 * 4096. Always lets a frame pass.\n"
                "local function wait(seconds)\n"
                "    local left\n"
                "    if type(seconds) == \"table\" then\n"
                "        left = seconds._raw * 30  -- a FixedPoint keeps 4096 per unit in _raw\n"
                "    else\n"
                "        left = seconds * 122880\n"
                "    end\n"
                "    repeat\n"
                "        left = left - coroutine.yield()\n"
                "    until left <= 0\n"
                "end\n";
    }
    std::vector<std::string> texts;
    for (Function& fn : functions) {
        if (fn.name == "onUpdate") {
            std::string resumes;
            for (const std::string& t : tasks)
                resumes += pad(1) + "if tasks." + t + " then\n" + pad(2) + "resumeTask(\"" + t + "\", dt)\n" + pad(1) + "end\n";
            if (!resumes.empty() && !fn.body.empty()) resumes += "\n";
            fn.body = resumes + fn.body;
            if (fn.onlyForTasks && tasks.empty()) continue;
        }
        texts.push_back("function " + fn.name + "(" + fn.params + ")\n" + fn.body + "end\n");
    }

    LuaGen out;
    out.lua = helpers;
    if (!helpers.empty() && !vars.empty()) out.lua += "\n";
    out.lua += vars;
    for (const std::string& t : texts) {
        if (!out.lua.empty()) out.lua += "\n";
        out.lua += t;
    }
    out.errors = std::move(g.errors);
    return out;
}

std::string blocksFileToLua(const std::string& json, const std::string& path) {
    LuaGen g = blocksToLua(parseBlocks(json, path));
    if (!g.errors.empty()) throw std::runtime_error(path + ": " + g.errors[0]);
    return g.lua;
}

}  // namespace splash
