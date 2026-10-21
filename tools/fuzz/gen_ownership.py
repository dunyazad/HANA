# Ownership fuzzer: generates valid programs that move, overwrite, loop over,
# match and drop values with heap data (Noisy structs, str, List), runs each
# with 'hana run --sanitize=address' and saves any program with a leak, a
# double free, a use after free or a nonzero exit as <out>/badN.hana.
# Programs the compiler rejects (use after move, ...) are skipped.
# usage: python3 tools/fuzz/gen_ownership.py <hana> <out dir> <seed> <count>
import random, subprocess, sys, os
hana, out, seed, count = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
os.makedirs(out, exist_ok=True)
random.seed(seed)
Q = "'" * 3
HEADER = """{
  Noisy: struct {
    name: str,
    drop: fn<(inout self)> print(f"drop {self.name}"),
  },
  Pair: struct {
    left: Noisy,
    right: str,
  },
  Locked: struct {
    inner: Noisy,
    drop: fn<(inout self)> print("unlock"),
  },
  Slot: enum {
    Full: { item: Noisy, tag: str },
    Empty: {},
  },
  make: fn<Noisy(n: i32)> Noisy { name: f"n{n}" },
  text: fn<str(n: i32)> f"t{n}",
  consume: fn<(sink n: Noisy)> print(f"consume {n.name}"),
  eat: fn<(sink s: str)> print(s),
  peek: fn<str(n: Noisy)> n.name.clone(),
  pass_on: fn<Noisy(sink n: Noisy)> n,
  pair: fn<Pair(k: i32)> Pair { left: make(k), right: text(k) },
  slot: fn<Slot(k: i32)> Slot.Full { item: make(k), tag: text(k) } if k % 2 == 0 else Slot.Empty,
"""
class Gen:
    def __init__(self):
        self.k = 0
        self.lines = []
    def n(self):
        self.k += 1
        return self.k
    def noisy_expr(self, env):
        c = random.random()
        live = [v for v, t in env if t == "Noisy"]
        if c < 0.4 or not live:
            return f"make({self.n()})"
        if c < 0.6:
            return f"pass_on(make({self.n()}))"
        if c < 0.75:
            return f"pair({self.n()}).left"
        if c < 0.85:
            return f"make({self.n()}) if {random.choice(['true', 'false'])} else make({self.n()})"
        return f"match slot({self.n()}):\n" + "{I}  Full(item, _): item\n{I}  Empty: make(" + str(self.n()) + ")"
    def str_expr(self, env):
        c = random.random()
        strs = [v for v, t in env if t == "str"]
        if c < 0.3 or not strs:
            return f"text({self.n()})"
        if c < 0.5:
            return f"{random.choice(strs)} + text({self.n()})"
        if c < 0.7:
            return f"{random.choice(strs)}.clone()"
        if c < 0.85:
            return f"f\"{{{random.choice(strs)}}}-{self.n()}\""
        return f"peek(make({self.n()}))"
    def block(self, env, depth, indent, loop):
        env = list(env)
        lines = []
        for _ in range(random.randint(1, 5 if depth == 0 else 3)):
            lines += self.stmt(env, depth, indent, loop)
        return lines
    def stmt(self, env, depth, indent, loop):
        I = "  " * indent
        c = random.random()
        name = f"v{self.n()}"
        def fmt(e):
            return e.replace("{I}", I)
        if random.random() < 0.12:
            if random.random() < 0.7:
                env.append((name, "List"))
                items = ", ".join(f"make({self.n()})" for _ in range(random.randint(0, 3)))
                return [I + f"var {name}: List<Noisy> = [{items}]"]
            env.append((name, "SList"))
            items = ", ".join(f"text({self.n()})" for _ in range(random.randint(0, 3)))
            return [I + f"var {name}: List<str> = [{items}]"]
        if c < 0.18:
            e = fmt(self.noisy_expr(env))
            env.append((name, "Noisy"))
            return [I + f"var {name} = " + e]
        if c < 0.28:
            e = fmt(self.str_expr(env))
            env.append((name, "str"))
            return [I + f"var {name} = " + e]
        if c < 0.33:
            env.append((name, "Pair"))
            return [I + f"var {name} = pair({self.n()})"]
        if c < 0.37:
            env.append((name, "Locked"))
            return [I + f"var {name} = Locked {{ inner: make({self.n()}) }}"]
        if c < 0.41:
            env.append((name, "Slot"))
            return [I + f"var {name} = slot({self.n()})"]
        if c < 0.40 and random.random() < 0.6:
            env.append((name, "List"))
            items = ", ".join(f"make({self.n()})" for _ in range(random.randint(0, 3)))
            return [I + f"var {name}: List<Noisy> = [{items}]"]
        if c < 0.41 and random.random() < 0.6:
            env.append((name, "SList"))
            items = ", ".join(f"text({self.n()})" for _ in range(random.randint(0, 3)))
            return [I + f"var {name}: List<str> = [{items}]"]
        if c < 0.44:
            env.append((name, "Opt"))
            return [I + f"var {name}: Noisy? = make({self.n()})" if random.random() < 0.7 else I + f"var {name}: Noisy? = null"]
        live = [(v, t) for v, t in env]
        if c < 0.56 and live:
            v, t = random.choice(live)
            if t == "Noisy":
                return [I + random.choice([f"consume({v})", f"{v} = " + fmt(self.noisy_expr(env)), f"print(peek({v}))", f"let {name} = {v}"])]
            if t == "str":
                return [I + random.choice([f"eat({v})", f"{v} += text({self.n()})", f"{v} += {v}", f"print({v}.len())", f"{v} = " + fmt(self.str_expr(env))])]
            if t == "Pair":
                return [I + random.choice([f"consume({v}.left)", f"eat({v}.right)", f"{v}.left = make({self.n()})", f"{v}.right += text({self.n()})", f"let {name} = {v}", f"print({v}.left.name)"])]
            if t == "Locked":
                return [I + random.choice([f"print({v}.inner.name)", f"{v}.inner = make({self.n()})", f"let {name} = {v}"])]
            if t == "Slot":
                if random.random() < 0.5:
                    return [I + f"match {v}:", I + "  Full(item, tag): print(f\"{item.name} {tag}\")", I + "  Empty: pass"]
                return [I + f"match {v}:", I + "  Full(item, _): consume(item)", I + "  Empty: pass"]
            if t == "List":
                k = self.n()
                return random.choice([
                    [I + f"{v}.push(make({k}))"],
                    [I + f"match {v}.pop():", I + "  Some(x): consume(x)", I + "  null: pass"],
                    [I + f"if {v}.len() > 0:", I + f"  {v}[0] = make({k})"],
                    [I + f"if {v}.len() > 1:", I + f"  consume({v}.remove(1))"],
                    [I + f"{v}.insert(0, make({k}))"],
                    [I + f"{v}.clear()"],
                    [I + f"for n in {v}:", I + "  print(n.name)"],
                    [I + f"for inout n in {v}:", I + f"  n = make({k})"],
                    [I + f"for sink n in {v}:", I + f"  if n.name == \"n{k % 7}\":", I + "    break", I + "  consume(n)"],
                    [I + f"let {name} = {v}"],
                    [I + f"if not {v}.is_empty():", I + f"  print(peek({v}[{v}.len() - 1]))"],
                ])
            if t == "SList":
                k = self.n()
                return random.choice([
                    [I + f"{v}.push(text({k}))"],
                    [I + f"if {v}.len() > 0:", I + f"  {v}[0] += text({k})"],
                    [I + f"let {name} = {v}.clone()"],
                    [I + f"for sink s in {v}.clone():", I + "  eat(s)"],
                    [I + f"for inout s in {v}:", I + f"  s += \"x\""],
                    [I + f"if {v}.len() > 0:", I + f"  eat({v}.remove(0))"],
                    [I + f"for s in {v}:", I + "  print(s)"],
                ])
            if t == "Opt":
                return [I + random.choice([f"{v} = null", f"{v} = make({self.n()})", f"print({v} != null)"]),] if random.random() < 0.6 else [I + f"match {v}:", I + "  Some(x): consume(x)", I + "  null: pass"]
        if c < 0.66 and depth < 3:
            cond = random.choice(["true", "false", f"peek(make({self.n()})) == \"n0\""])
            lines = [I + f"if {cond}:"] + self.block(env, depth + 1, indent + 1, loop)
            if random.random() < 0.5:
                lines += [I + "else:"] + self.block(env, depth + 1, indent + 1, loop)
            return lines
        if c < 0.74 and depth < 3:
            i = f"i{self.n()}"
            body = self.block(env, depth + 1, indent + 1, True)
            lines = [I + f"for {i} in 0..{random.randint(0, 3)}:"]
            if random.random() < 0.4:
                lines.append(I + f"  if {i} == 1:")
                lines.append(I + "    " + random.choice(["break", "continue"]))
            return lines + body
        if c < 0.78 and loop:
            return [I + random.choice(["break", "continue"])]
        if c < 0.84:
            return [I + f"make({self.n()})", I + f"print(peek(make({self.n()})))"]
        if c < 0.88:
            return [I + f"let ({name}, _) = (make({self.n()}), text({self.n()}))"]
        if c < 0.92:
            return [I + f"match slot({self.n()}):", I + "  Full(item, _): consume(item)", I + "  Empty: pass"]
        if c < 0.95 and depth > 0:
            return [I + "return"]
        return [I + f"print(text({self.n()}) + text({self.n()}))"]
def program():
    g = Gen()
    fns = []
    for f in range(random.randint(1, 3)):
        body = g.block([], 0, 2, False)
        fns.append(f"  f{f}: fn " + Q + "\n" + "\n".join("  " + l for l in "\n".join(body).split("\n")) + "\n  " + Q + ",\n")
    main = "\n".join(f"    f{f}()" for f in range(len(fns)))
    return HEADER + "".join(fns) + "  main: fn " + Q + "\n" + main + "\n  " + Q + ",\n}\n"
found = ran = 0
for i in range(count):
    text = program()
    path = os.path.join(out, "gen.hana")
    open(path, "w").write(text)
    r = subprocess.run([hana, "run", "--sanitize=address", path], capture_output=True, timeout=60)
    if r.returncode == 1 and b"error:" in r.stderr and b"Sanitizer" not in r.stderr:
        continue
    ran += 1
    if r.returncode != 0 or b"Sanitizer" in r.stderr or b"runtime error" in r.stderr:
        found += 1
        open(os.path.join(out, f"bad{found}.hana"), "w").write(text)
        print("BAD", r.returncode, r.stderr[-1500:].decode(errors="replace"))
print("done, ran:", ran, "of", count, "found:", found)
