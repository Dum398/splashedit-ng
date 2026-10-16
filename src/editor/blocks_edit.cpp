#include "editor/blocks_edit.hh"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace editor::blk {

using splash::BlockField;
using splash::BlockSpec;
using splash::findBlockSpec;

// --- Paths -------------------------------------------------------------------

std::string Path::str() const {
    std::string s = "s" + std::to_string(stack);
    for (const Step& st : steps) s += std::string(".") + st.kind + std::to_string(st.index);
    return s;
}

Path Path::then(char kind, int index) const {
    Path p = *this;
    p.steps.push_back({kind, index});
    return p;
}

Path Path::parent() const {
    if (steps.empty()) return {};
    Path p = *this;
    if (p.steps.back().kind == 'a') {
        p.steps.pop_back();
        return p;
    }
    p.steps.pop_back();
    if (p.steps.empty()) return {};
    if (p.steps.back().kind == 'd') p.steps.pop_back();
    return p;
}

Path parsePath(std::string_view s) {
    Path p;
    size_t i = 0;
    auto number = [&](int* out) {
        size_t start = i;
        long v = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9' && i - start < 6) v = v * 10 + (s[i++] - '0');
        *out = int(v);
        return i > start;
    };
    if (s.empty() || s[0] != 's') return {};
    i = 1;
    int stack = 0;
    if (!number(&stack)) return {};
    p.stack = stack;
    while (i < s.size()) {
        if (s[i] != '.' || i + 1 >= s.size()) return {};
        char k = s[i + 1];
        if (k != 'b' && k != 'a' && k != 'd') return {};
        i += 2;
        int n = 0;
        if (!number(&n)) return {};
        p.steps.push_back({k, n});
    }
    return p;
}

namespace {

// The list a path leads to: the stack's own list, or a body ('d' last).
const std::vector<Block>* listAt(const BlockScript& s, int stack, const std::vector<Step>& steps, size_t count) {
    if (stack < 0 || size_t(stack) >= s.stacks.size()) return nullptr;
    const std::vector<Block>* list = &s.stacks[size_t(stack)].blocks;
    const Block* b = nullptr;
    for (size_t k = 0; k < count; k++) {
        const Step& st = steps[k];
        if (st.index < 0) return nullptr;
        size_t i = size_t(st.index);
        if (st.kind == 'b') {
            if (!list || i >= list->size()) return nullptr;
            b = &(*list)[i];
            list = nullptr;
        } else if (st.kind == 'a') {
            if (!b || i >= b->args.size()) return nullptr;
            b = &b->args[i];
        } else {
            if (!b || i >= b->bodies.size()) return nullptr;
            list = &b->bodies[i];
            b = nullptr;
        }
    }
    return list;
}

}  // namespace

const Block* blockAt(const BlockScript& s, const Path& p) {
    if (p.steps.empty() || p.steps.back().kind == 'd') return nullptr;
    // The last step from the list or block before it.
    const Step& last = p.steps.back();
    if (last.index < 0) return nullptr;
    if (last.kind == 'b') {
        const std::vector<Block>* list = listAt(s, p.stack, p.steps, p.steps.size() - 1);
        return list && size_t(last.index) < list->size() ? &(*list)[size_t(last.index)] : nullptr;
    }
    Path up = p;
    up.steps.pop_back();
    const Block* owner = blockAt(s, up);
    return owner && size_t(last.index) < owner->args.size() ? &owner->args[size_t(last.index)] : nullptr;
}

Block* blockAt(BlockScript& s, const Path& p) { return const_cast<Block*>(blockAt(static_cast<const BlockScript&>(s), p)); }

static std::vector<Block>* listOf(BlockScript& s, const Path& p) {
    // The list holding the block at p ('b' last).
    if (!p.inList()) return nullptr;
    return const_cast<std::vector<Block>*>(listAt(s, p.stack, p.steps, p.steps.size() - 1));
}

bool isLiteral(const Block& b) { return b.op == "number" || b.op == "text" || b.op == "bool"; }

bool booleanSlot(std::string_view op, int arg) {
    if (op == "if" || op == "if_else" || op == "while" || op == "repeat_until" || op == "wait_until" || op == "not") return arg == 0;
    if (op == "and" || op == "or") return arg == 0 || arg == 1;
    return false;
}

Block newBlock(std::string_view op) {
    Block b;
    b.op = std::string(op);
    const BlockSpec* spec = findBlockSpec(op);
    if (!spec) return b;
    switch (spec->field) {
        case BlockField::Object: b.field = "self"; break;
        case BlockField::Button: b.field = "CROSS"; break;
        case BlockField::Bool: b.field = "true"; break;
        case BlockField::Number: b.field = "0"; break;
        default: break;
    }
    auto num = [](const char* v) { return Block{"number", v, {}, {}}; };
    auto txt = [](const char* v) { return Block{"text", v, {}, {}}; };
    for (int i = 0; i < spec->args; i++) {
        if (booleanSlot(op, i)) b.args.push_back(Block{});
        else if (op == "log" || op == "join" || op == "load_scene") b.args.push_back(txt(""));
        else if (op == "change_var" || op == "wait") b.args.push_back(num("1"));
        else if (op == "repeat") b.args.push_back(num("10"));
        else if (op == "random") b.args.push_back(num(i == 0 ? "1" : "10"));
        else b.args.push_back(num("0"));
    }
    b.bodies.resize(size_t(spec->bodies));
    return b;
}

// --- Layout -------------------------------------------------------------------

const BlockBox* Layout::find(const Path& p) const {
    for (const BlockBox& b : blocks)
        if (b.path == p) return &b;
    return nullptr;
}

namespace {

struct Tok {
    enum Kind { Text, Field, Arg } kind;
    std::string text;
    int arg = -1;
};

std::vector<std::string_view> labelParts(std::string_view label) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    for (;;) {
        size_t bar = label.find('|', start);
        parts.push_back(label.substr(start, bar == std::string_view::npos ? std::string_view::npos : bar - start));
        if (bar == std::string_view::npos) break;
        start = bar + 1;
    }
    return parts;
}

std::vector<Tok> tokens(std::string_view part) {
    std::vector<Tok> out;
    size_t i = 0;
    while (i < part.size()) {
        while (i < part.size() && part[i] == ' ') i++;
        if (i >= part.size()) break;
        size_t e = part.find(' ', i);
        if (e == std::string_view::npos) e = part.size();
        std::string_view w = part.substr(i, e - i);
        i = e;
        if (w == "%f") out.push_back({Tok::Field, {}, -1});
        else if (w.size() == 2 && w[0] == '%' && w[1] >= '1' && w[1] <= '9') out.push_back({Tok::Arg, {}, w[1] - '1'});
        else if (!out.empty() && out.back().kind == Tok::Text) out.back().text += " " + std::string(w);
        else out.push_back({Tok::Text, std::string(w), -1});
    }
    return out;
}

const char* placeholder(BlockField f) {
    switch (f) {
        case BlockField::Object: return "object";
        case BlockField::Variable: return "variable";
        case BlockField::Button: return "button";
        case BlockField::Sound: return "sound";
        case BlockField::Cutscene: return "cutscene";
        default: return "";
    }
}

struct Sz {
    float w = 0, h = 0;
};

struct Layouter {
    const TextWidth& tw;
    const Metrics& m;
    Layout* out = nullptr;

    bool reporterKind(BlockKind k) const { return k == BlockKind::Reporter || k == BlockKind::Boolean; }

    std::string fieldText(const Block& b, const BlockSpec* s, bool* ph) const {
        *ph = b.field.empty() && s && *placeholder(s->field);
        return *ph ? placeholder(s->field) : b.field;
    }
    float wellW(const std::string& t) const { return std::max(m.slotW, std::ceil(tw(t)) + 2 * m.wellPad); }

    Sz pieceSize(const Tok& t, const Block& b, const BlockSpec* s) const {
        if (t.kind == Tok::Text) return {std::ceil(tw(t.text)), m.text};
        if (t.kind == Tok::Field) {
            bool ph = false;
            return {wellW(fieldText(b, s, &ph)), m.slotH};
        }
        if (t.arg >= 0 && size_t(t.arg) < b.args.size() && !b.args[size_t(t.arg)].op.empty()) return measure(b.args[size_t(t.arg)]);
        return {m.slotW, m.slotH};
    }

    // Width and height of a label row.
    Sz rowSize(const std::vector<Tok>& toks, const Block& b, const BlockSpec* s, float padX, float minH, float padY) const {
        float w = 0, h = 0;
        for (size_t i = 0; i < toks.size(); i++) {
            Sz p = pieceSize(toks[i], b, s);
            w += p.w + (i ? m.gap : 0);
            h = std::max(h, p.h);
        }
        return {w + 2 * padX, std::max(minH, h + 2 * padY)};
    }

    float padXOf(const BlockSpec* s) const {
        if (!s) return m.padX;
        if (s->kind == BlockKind::Boolean) return m.reporterPadX + 4;
        if (s->kind == BlockKind::Reporter) return m.reporterPadX;
        return m.padX;
    }

    Sz measureList(const std::vector<Block>& list) const {
        Sz z;
        for (const Block& b : list) {
            Sz s = measure(b);
            z.w = std::max(z.w, s.w);
            z.h += s.h;
        }
        return z;
    }

    Sz measure(const Block& b) const {
        const BlockSpec* s = findBlockSpec(b.op);
        if (b.op.empty()) return {m.slotW, m.slotH};
        if (isLiteral(b)) {
            bool ph = false;
            return {wellW(fieldText(b, s, &ph)), m.slotH};
        }
        if (!s) return {std::max(m.minW, std::ceil(tw(b.op)) + 2 * m.padX), m.row};
        std::vector<std::string_view> parts = labelParts(s->label);
        if (reporterKind(s->kind)) return rowSize(tokens(parts[0]), b, s, padXOf(s), m.slotH + 4, 2);
        Sz z;
        for (size_t k = 0; k < parts.size(); k++) {
            Sz r = rowSize(tokens(parts[k]), b, s, m.padX, m.row, m.padY);
            z.w = std::max(z.w, r.w);
            z.h += r.h;
        }
        z.w = std::max(z.w, m.minW);
        if (s->kind == BlockKind::Hat) z.h += m.hatTop;
        if (s->kind == BlockKind::CBlock) {
            for (int d = 0; d < s->bodies; d++) {
                const std::vector<Block> none;
                Sz body = measureList(size_t(d) < b.bodies.size() ? b.bodies[size_t(d)] : none);
                z.h += std::max(m.mouthMin, body.h);
            }
            z.h += m.arm;
            z.w = std::max(z.w, m.minW * 2);
        }
        return z;
    }

    // Lays out one label row at x, y; pieces go into `box`, inputs to place later into `args`.
    void placeRow(const std::vector<Tok>& toks, const Block& b, const BlockSpec* s, float x, float y, float h, float padX,
                  BlockBox& box, std::vector<std::pair<int, Rect>>& args) const {
        float cx = x + padX;
        for (size_t i = 0; i < toks.size(); i++) {
            if (i) cx += m.gap;
            Sz p = pieceSize(toks[i], b, s);
            Piece pc;
            pc.rect = {cx, y + std::round((h - p.h) * 0.5f), cx + p.w, y + std::round((h - p.h) * 0.5f) + p.h};
            if (toks[i].kind == Tok::Text) {
                pc.kind = Piece::Text;
                pc.text = toks[i].text;
            } else if (toks[i].kind == Tok::Field) {
                pc.kind = Piece::Field;
                pc.text = fieldText(b, s, &pc.placeholder);
                box.hasField = true;
                box.field = pc.rect;
            } else {
                pc.kind = Piece::Slot;
                pc.arg = toks[i].arg;
                pc.filled = size_t(pc.arg) < b.args.size() && !b.args[size_t(pc.arg)].op.empty();
                if (size_t(pc.arg) >= box.slots.size()) box.slots.resize(size_t(pc.arg) + 1);
                box.slots[size_t(pc.arg)] = pc.rect;
                if (pc.filled) args.push_back({pc.arg, pc.rect});
            }
            box.pieces.push_back(pc);
            cx += p.w;
        }
    }

    // Places `b` with its top left at x, y; returns its height.
    float place(const Block& b, const Path& path, float x, float y, int depth) {
        const BlockSpec* s = findBlockSpec(b.op);
        Sz z = measure(b);
        BlockBox box;
        box.path = path;
        box.op = b.op;
        box.kind = s ? s->kind : (b.op.empty() ? BlockKind::Reporter : BlockKind::Statement);
        box.category = s ? s->category : std::string_view();
        box.rect = {x, y, x + z.w, y + z.h};
        box.depth = depth;
        std::vector<std::pair<int, Rect>> args;
        std::vector<std::pair<int, float>> bodies;  // body, top
        if (b.op.empty()) {
            box.parts.push_back(box.rect);
        } else if (isLiteral(b)) {
            box.parts.push_back(box.rect);
            box.hasField = true;
            box.field = box.rect;
            Piece pc;
            pc.kind = Piece::Field;
            pc.rect = box.rect;
            pc.text = fieldText(b, s, &pc.placeholder);
            box.pieces.push_back(pc);
        } else if (!s) {
            box.parts.push_back(box.rect);
            Piece pc;
            pc.text = b.op;
            pc.rect = {x + m.padX, y + (z.h - m.text) * 0.5f, x + z.w - m.padX, y + (z.h + m.text) * 0.5f};
            box.pieces.push_back(pc);
        } else {
            std::vector<std::string_view> parts = labelParts(s->label);
            float cy = y;
            if (s->kind == BlockKind::Hat) cy += m.hatTop;
            for (size_t k = 0; k < parts.size(); k++) {
                std::vector<Tok> toks = tokens(parts[k]);
                bool rep = reporterKind(s->kind);
                Sz r = rep ? rowSize(toks, b, s, padXOf(s), m.slotH + 4, 2) : rowSize(toks, b, s, m.padX, m.row, m.padY);
                float rowTop = k == 0 ? y : cy;  // a hat's first row takes in its cap
                box.parts.push_back({x, rowTop, x + z.w, cy + r.h});
                placeRow(toks, b, s, x, cy, r.h, padXOf(s), box, args);
                cy += r.h;
                if (s->kind == BlockKind::CBlock && int(k) < s->bodies) {
                    const std::vector<Block> none;
                    Sz body = measureList(k < b.bodies.size() ? b.bodies[k] : none);
                    float mh = std::max(m.mouthMin, body.h);
                    box.mouths.push_back({x + m.mouthIndent, cy, x + z.w, cy + mh});
                    // The arm left of the mouth is the block's own.
                    box.parts.push_back({x, cy, x + m.mouthIndent, cy + mh});
                    if (k < b.bodies.size()) bodies.push_back({int(k), cy});
                    cy += mh;
                }
            }
            if (s->kind == BlockKind::CBlock) {
                // Bodies past the label's parts (a label without enough '|').
                while (int(box.mouths.size()) < s->bodies) {
                    box.mouths.push_back({x + m.mouthIndent, cy, x + z.w, cy + m.mouthMin});
                    box.parts.push_back({x, cy, x + m.mouthIndent, cy + m.mouthMin});
                    cy += m.mouthMin;
                }
                box.parts.push_back({x, cy, x + z.w, cy + m.arm});
            }
            if (box.slots.size() < size_t(s->args)) box.slots.resize(size_t(s->args));
        }
        out->blocks.push_back(std::move(box));
        for (const auto& [i, r] : args) place(b.args[size_t(i)], path.then('a', i), r.x0, r.y0, depth + 1);
        for (const auto& [d, top] : bodies) placeList(b.bodies[size_t(d)], path.then('d', d), x + m.mouthIndent, top, depth + 1);
        return z.h;
    }

    // `owner` is the list's path (a stack's: no steps; a body: 'd' last).
    float placeList(const std::vector<Block>& list, const Path& owner, float x, float y, int depth) {
        float cy = y;
        const bool body = !owner.steps.empty();
        for (size_t i = 0; i < list.size(); i++) {
            const size_t at = out->blocks.size();
            cy += place(list[i], owner.then('b', int(i)), x, cy, depth);
            BlockBox& b = out->blocks[at];
            b.above = i > 0 || body;
            b.below = i + 1 < list.size() || body;
            b.inBody = body;
        }
        return cy - y;
    }
};

}  // namespace

Layout layout(const BlockScript& s, const TextWidth& tw, const Metrics& m) {
    Layout l;
    Layouter L{tw, m, &l};
    for (size_t si = 0; si < s.stacks.size(); si++) {
        const BlockStack& st = s.stacks[si];
        const size_t first = l.blocks.size();
        Path root;
        root.stack = int(si);
        float h = L.placeList(st.blocks, root, st.x, st.y, 0);
        StackBox sb;
        sb.stack = int(si);
        float w = 0;
        for (size_t k = first; k < l.blocks.size(); k++) w = std::max(w, l.blocks[k].rect.x1 - st.x);
        sb.rect = {st.x, st.y, st.x + w, st.y + h};
        sb.bottomX = st.x;
        sb.bottomY = st.y + h;
        const BlockSpec* top = st.blocks.empty() ? nullptr : findBlockSpec(st.blocks[0].op);
        sb.hat = top && top->kind == BlockKind::Hat;
        l.stacks.push_back(sb);
    }
    return l;
}

std::pair<float, float> stackSize(const BlockStack& st, const TextWidth& tw, const Metrics& m) {
    BlockScript one;
    one.stacks.push_back({0, 0, st.blocks});
    Layout l = layout(one, tw, m);
    return {l.stacks[0].rect.w(), l.stacks[0].rect.h()};
}

Hit hitTest(const Layout& l, float x, float y) {
    for (size_t k = l.blocks.size(); k-- > 0;) {
        const BlockBox& b = l.blocks[k];
        bool on = false;
        for (const Rect& r : b.parts) on = on || r.contains(x, y);
        if (!on) continue;
        Hit h;
        h.path = b.path;
        if (b.hasField && b.field.contains(x, y)) {
            h.kind = Hit::Field;
            return h;
        }
        for (const Piece& p : b.pieces)
            if (p.kind == Piece::Slot && !p.filled && p.rect.contains(x, y)) {
                h.kind = Hit::Slot;
                h.slot = p.arg;
                return h;
            }
        h.kind = Hit::Block;
        return h;
    }
    return {};
}

// --- Edits ------------------------------------------------------------------------

std::optional<Taken> takeStack(const BlockScript& s, const Path& p) {
    if (!blockAt(s, p)) return std::nullopt;
    Taken t;
    t.rest = s;
    if (p.steps.back().kind == 'a') {
        Block* b = blockAt(t.rest, p);
        t.fragment.blocks.push_back(std::move(*b));
        *b = Block{};
        return t;
    }
    std::vector<Block>* list = listOf(t.rest, p);
    const size_t i = size_t(p.steps.back().index);
    t.fragment.blocks.assign(std::make_move_iterator(list->begin() + std::ptrdiff_t(i)), std::make_move_iterator(list->end()));
    list->erase(list->begin() + std::ptrdiff_t(i), list->end());
    if (p.steps.size() == 1 && i == 0) t.rest.stacks.erase(t.rest.stacks.begin() + p.stack);
    return t;
}

std::optional<BlockScript> deleteFragment(const BlockScript& s, const Path& p) {
    std::optional<Taken> t = takeStack(s, p);
    if (!t) return std::nullopt;
    return std::move(t->rest);
}

namespace {

BlockKind kindOf(const Block& b) {
    const BlockSpec* s = findBlockSpec(b.op);
    return s ? s->kind : BlockKind::Statement;
}

bool statementKind(BlockKind k) { return k == BlockKind::Statement || k == BlockKind::CBlock; }

// Input `arg` of the block at p can take `r`.
bool slotTakes(const BlockScript& s, const Path& p, int arg, const Block& r) {
    const Block* b = blockAt(s, p);
    if (!b) return false;
    const BlockSpec* spec = findBlockSpec(b->op);
    const BlockSpec* rs = findBlockSpec(r.op);
    if (!spec || !rs || arg < 0 || arg >= spec->args) return false;
    if (rs->kind != BlockKind::Reporter && rs->kind != BlockKind::Boolean) return false;
    // A variable may hold true or false: it goes where a condition does.
    return !booleanSlot(b->op, arg) || rs->kind == BlockKind::Boolean || r.op == "var";
}

}  // namespace

bool canDrop(const BlockScript& s, const BlockStack& f, const Target& t) {
    if (f.blocks.empty() || t.kind == Target::None) return false;
    if (t.kind == Target::Canvas) return true;
    const BlockSpec* top = findBlockSpec(f.blocks[0].op);
    if (!top) return false;
    const bool endsStop = f.blocks.back().op == "stop";
    auto looseStatementStack = [&](int si) {
        if (si < 0 || size_t(si) >= s.stacks.size() || s.stacks[size_t(si)].blocks.empty()) return false;
        return statementKind(kindOf(s.stacks[size_t(si)].blocks[0]));
    };
    switch (top->kind) {
        case BlockKind::Hat:
            return t.kind == Target::StackTop && t.path.steps.empty() && looseStatementStack(t.path.stack) && !endsStop;
        case BlockKind::Statement:
        case BlockKind::CBlock: {
            for (size_t i = 1; i < f.blocks.size(); i++)
                if (!statementKind(kindOf(f.blocks[i]))) return false;
            if (t.kind == Target::StackTop) return t.path.steps.empty() && looseStatementStack(t.path.stack) && !endsStop;
            if (t.kind == Target::After) {
                const Block* b = blockAt(s, t.path);
                if (!b || !t.path.inList() || b->op == "stop") return false;
                BlockKind k = kindOf(*b);
                if (k == BlockKind::Reporter || k == BlockKind::Boolean) return false;
                if (k == BlockKind::Hat && t.path.steps.size() != 1) return false;
                const std::vector<Block>* list = listAt(s, t.path.stack, t.path.steps, t.path.steps.size() - 1);
                const bool below = list && size_t(t.path.steps.back().index) + 1 < list->size();
                return !(endsStop && below);
            }
            if (t.kind == Target::BodyTop) {
                const Block* b = blockAt(s, t.path);
                const BlockSpec* bs = b ? findBlockSpec(b->op) : nullptr;
                if (!bs || bs->kind != BlockKind::CBlock || t.index < 0 || t.index >= bs->bodies) return false;
                const bool below = size_t(t.index) < b->bodies.size() && !b->bodies[size_t(t.index)].empty();
                return !(endsStop && below);
            }
            return false;
        }
        case BlockKind::Reporter:
        case BlockKind::Boolean:
            return f.blocks.size() == 1 && t.kind == Target::Slot && slotTakes(s, t.path, t.index, f.blocks[0]);
    }
    return false;
}

std::optional<BlockScript> dropReporter(const BlockScript& s, const Block& reporter, const Path& p, int arg, float x, float y) {
    if (!slotTakes(s, p, arg, reporter)) return std::nullopt;
    BlockScript r = s;
    Block* b = blockAt(r, p);
    const BlockSpec* spec = findBlockSpec(b->op);
    if (b->args.size() < size_t(spec->args)) b->args.resize(size_t(spec->args));
    Block old = std::move(b->args[size_t(arg)]);
    b->args[size_t(arg)] = reporter;
    if (!old.op.empty() && !isLiteral(old)) r.stacks.push_back({x, y, {std::move(old)}});
    return r;
}

std::optional<BlockScript> dropStack(const BlockScript& s, const BlockStack& f, const Target& t, Path* placed) {
    if (!canDrop(s, f, t)) return std::nullopt;
    BlockScript r = s;
    Path at;
    switch (t.kind) {
        case Target::Canvas:
            r.stacks.push_back({t.x, t.y, f.blocks});
            at.stack = int(r.stacks.size()) - 1;
            at.steps = {{'b', 0}};
            break;
        case Target::After: {
            std::vector<Block>* list = listOf(r, t.path);
            const int i = t.path.steps.back().index + 1;
            list->insert(list->begin() + i, f.blocks.begin(), f.blocks.end());
            at = t.path;
            at.steps.back().index = i;
            break;
        }
        case Target::BodyTop: {
            Block* b = blockAt(r, t.path);
            const BlockSpec* bs = findBlockSpec(b->op);
            if (b->bodies.size() < size_t(bs->bodies)) b->bodies.resize(size_t(bs->bodies));
            std::vector<Block>& body = b->bodies[size_t(t.index)];
            body.insert(body.begin(), f.blocks.begin(), f.blocks.end());
            at = t.path.then('d', t.index).then('b', 0);
            break;
        }
        case Target::StackTop: {
            BlockStack& st = r.stacks[size_t(t.path.stack)];
            st.blocks.insert(st.blocks.begin(), f.blocks.begin(), f.blocks.end());
            st.x = t.x;
            st.y = t.y;
            at.stack = t.path.stack;
            at.steps = {{'b', 0}};
            break;
        }
        case Target::Slot: {
            std::optional<BlockScript> d = dropReporter(s, f.blocks[0], t.path, t.index, t.x, t.y);
            if (!d) return std::nullopt;
            r = std::move(*d);
            at = t.path.then('a', t.index);
            break;
        }
        case Target::None: return std::nullopt;
    }
    if (placed) *placed = at;
    return r;
}

std::optional<BlockScript> setField(const BlockScript& s, const Path& p, const std::string& value) {
    const Block* b = blockAt(s, p);
    const BlockSpec* spec = b ? findBlockSpec(b->op) : nullptr;
    if (!spec || spec->field == BlockField::None) return std::nullopt;
    BlockScript r = s;
    blockAt(r, p)->field = value;
    return r;
}

Target findTarget(const BlockScript& s, const Layout& l, const BlockStack& f, float fx, float fy, const TextWidth& tw,
                  const Metrics& m, float radius) {
    Target best;
    if (f.blocks.empty()) return best;
    const BlockSpec* top = findBlockSpec(f.blocks[0].op);
    if (!top) return best;
    const float fh = stackSize(f, tw, m).second;
    float bestD = radius;
    auto offer = [&](const Target& t, float ax, float ay, float px, float py) {
        const float d = std::hypot(ax - px, ay - py);
        if (d > bestD || !canDrop(s, f, t)) return;
        bestD = d;
        best = t;
    };
    if (top->kind == BlockKind::Reporter || top->kind == BlockKind::Boolean) {
        for (const BlockBox& b : l.blocks)
            for (size_t i = 0; i < b.slots.size(); i++) {
                const Rect& r = b.slots[i];
                if (r.w() <= 0) continue;
                Target t{Target::Slot, b.path, int(i), r.x0 + 24, b.rect.y1 + 16};
                offer(t, r.x0, r.y0 + r.h() * 0.5f, fx, fy + fh * 0.5f);
            }
        return best;
    }
    // Over a loose stack: the fragment's bottom meets the stack's top.
    for (const StackBox& sb : l.stacks) {
        Path p;
        p.stack = sb.stack;
        offer(Target{Target::StackTop, p, 0, sb.rect.x0, sb.rect.y0 - fh}, sb.rect.x0, sb.rect.y0, fx, fy + fh);
    }
    if (top->kind == BlockKind::Hat) return best;
    for (const BlockBox& b : l.blocks) {
        if (b.path.inList()) offer(Target{Target::After, b.path, 0, 0, 0}, b.rect.x0, b.rect.y1, fx, fy);
        for (size_t d = 0; d < b.mouths.size(); d++)
            offer(Target{Target::BodyTop, b.path, int(d), 0, 0}, b.mouths[d].x0, b.mouths[d].y0, fx, fy);
    }
    return best;
}

// --- Variables ----------------------------------------------------------------

namespace {

bool namesVariable(const Block& b, std::string_view name) {
    const BlockSpec* spec = findBlockSpec(b.op);
    return spec && spec->field == BlockField::Variable && b.field == name;
}

// Calls f on every block of the script, inputs and bodies included.
template <class List, class F>
void eachBlock(List& list, const F& f) {
    for (auto& b : list) {
        f(b);
        eachBlock(b.args, f);
        for (auto& body : b.bodies) eachBlock(body, f);
    }
}

int findVariable(const BlockScript& s, std::string_view name) {
    for (size_t i = 0; i < s.variables.size(); i++)
        if (s.variables[i].name == name) return int(i);
    return -1;
}

}  // namespace

int variableUses(const BlockScript& s, std::string_view name) {
    int n = 0;
    for (const BlockStack& st : s.stacks)
        eachBlock(st.blocks, [&](const Block& b) {
            if (namesVariable(b, name)) n++;
        });
    return n;
}

std::string usedBy(int uses) { return "used by " + std::to_string(uses) + (uses == 1 ? " block" : " blocks"); }

BlockScript addVariable(const BlockScript& s, std::string* name) {
    std::string n = "my_variable";
    for (int k = 2; findVariable(s, n) >= 0; k++) n = "my_variable" + std::to_string(k);
    BlockScript r = s;
    r.variables.push_back(splash::BlockVariable{n, "0"});
    if (name) *name = n;
    return r;
}

std::optional<BlockScript> renameVariable(const BlockScript& s, std::string_view from, std::string_view to,
                                          std::string* why) {
    auto refuse = [&](std::string w) -> std::optional<BlockScript> {
        if (why) *why = std::move(w);
        return std::nullopt;
    };
    const int i = findVariable(s, from);
    if (i < 0) return refuse("no variable \"" + std::string(from) + "\"");
    if (from == to) return s;
    std::string bad = splash::blockNameError(to);
    if (!bad.empty()) return refuse(bad);
    if (findVariable(s, to) >= 0) return refuse("there is a variable \"" + std::string(to) + "\" already");
    BlockScript r = s;
    r.variables[size_t(i)].name = std::string(to);
    for (BlockStack& st : r.stacks)
        eachBlock(st.blocks, [&](Block& b) {
            if (namesVariable(b, from)) b.field = std::string(to);
        });
    return r;
}

std::optional<BlockScript> setVariableValue(const BlockScript& s, int i, const std::string& value) {
    if (i < 0 || size_t(i) >= s.variables.size()) return std::nullopt;
    BlockScript r = s;
    r.variables[size_t(i)].value = value;
    return r;
}

std::optional<BlockScript> deleteVariable(const BlockScript& s, int i, std::string* why) {
    if (i < 0 || size_t(i) >= s.variables.size()) {
        if (why) *why = "no such variable";
        return std::nullopt;
    }
    const int uses = variableUses(s, s.variables[size_t(i)].name);
    if (uses > 0) {
        if (why) *why = usedBy(uses);
        return std::nullopt;
    }
    BlockScript r = s;
    r.variables.erase(r.variables.begin() + i);
    return r;
}

// --- Undo -------------------------------------------------------------------------

bool History::commit(BlockScript next) {
    if (next == m_now) return false;
    m_past.push_back(std::move(m_now));
    if (m_past.size() > kMax) m_past.erase(m_past.begin());
    m_now = std::move(next);
    m_future.clear();
    return true;
}

bool History::undo() {
    if (m_past.empty()) return false;
    m_future.push_back(std::move(m_now));
    m_now = std::move(m_past.back());
    m_past.pop_back();
    return true;
}

bool History::redo() {
    if (m_future.empty()) return false;
    m_past.push_back(std::move(m_now));
    m_now = std::move(m_future.back());
    m_future.pop_back();
    return true;
}

void History::reset(BlockScript s) {
    m_past.clear();
    m_future.clear();
    m_now = std::move(s);
}

}  // namespace editor::blk
