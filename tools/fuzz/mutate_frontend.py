# Front-end crash fuzzer: mutates the test and example sources with
# random HANA fragments and runs check/fmt/emit-ir on each mutant. Any exit
# code other than 0, 1 or 2, a sanitizer report or an internal compiler
# error is saved as <out>/crashN.hana. Run from the repository root with an
# AddressSanitizer/UBSan build of the compiler.
# usage: python3 tools/fuzz/mutate_frontend.py <hana> <out dir> [seed]
import random, subprocess, sys, os
hana = sys.argv[1]; out = sys.argv[2]
os.makedirs(out, exist_ok=True)
seeds = []
for root in ["tests/e2e/run", "tests/e2e/errors", "examples"]:
    for f in os.listdir(root):
        if f.endswith(".hana"):
            seeds.append(open(os.path.join(root, f), encoding="utf-8").read())
pieces = ["fn", "<", ">", "(", ")", "{", "}", "[", "]", "\"", "f\"{x}\"", "'", "'''", "\n", "  ", "\t",
          ",", ":", "&", "-", "inout", "let", "var", "=", "+=", "if", "elif", "else", "while", "break",
          "return", "0x", "1e", "//", "/*", "*/", "#", "\\", "\\u{", "\\ud83d", "\xed\x95", "main",
          "print", "as", "not", "and", "pass", "1..", "?", "<<", "i64", "void", "struct", "copy", "self", "sink", ".", "P {", "x: 1", "self.", "= 1", "Player", ".new(", "}", "enum", "match", "match x:\n", "  _: ", "Circle(r)", "..=", "..", " if ", "Shape.", "{}", "Red", "for ", " in ", "for i in 0..", ":\n  ", "(1, 2)", ".0", ".1", "let (a, b) = ", "(i32, i32)", ",)", "null", "Some(", "i32?", "Option<", ">>", "Some(_)", "Some(null)", "== null", "X(1)", "(_, _, _)", "[", "]", "[1, 2]", "List<", "xs[0]", "xs[i]", ".push(", ".pop()", ".remove(0)", ".insert(0, ", "for inout x in ", "for sink x in ", "for x in xs:\n", "[]", "List<str>", "<T>", "<T: Ord>", "T", "Pair<", "trait {", "is: [", "Shape", "Self", ": Clone", " + Copy", "<i32>", "max(", ".value_or(", "Maybe.None", "Box {", "?", "Ok(", "Err(", "Result<", "Result<i32, str>", ")?", ".is_ok()"]
random.seed(int(sys.argv[3]) if len(sys.argv) > 3 else 4321)
crashes = 0
for i in range(2000):
    s = list(random.choice(seeds))
    for _ in range(random.randint(1, 8)):
        op = random.random()
        pos = random.randint(0, len(s))
        if op < 0.45:
            s[pos:pos] = list(random.choice(pieces))
        elif op < 0.75 and s:
            del s[pos:pos + random.randint(1, 6)]
        else:
            s[pos:pos] = [chr(random.randint(0, 127))]
    text = "".join(s)
    path = os.path.join(out, "fuzz.hana")
    open(path, "w", encoding="utf-8", errors="surrogateescape").write(text)
    for cmd in (["check", "--tokens", "--ast", path], ["check", "--format", "json", path], ["fmt", "--stdout", path], ["build", "--emit-ir", path]):
        r = subprocess.run([hana] + cmd, capture_output=True, timeout=30)
        bad = r.returncode not in (0, 1, 2) or b"AddressSanitizer" in r.stderr or b"runtime error" in r.stderr or b"internal compiler error" in r.stderr
        if bad:
            crashes += 1
            open(os.path.join(out, f"crash{crashes}.hana"), "w", encoding="utf-8", errors="surrogateescape").write(text)
            print("CRASH", cmd[0], r.returncode, r.stderr[-600:])
            break
print("done, crashes:", crashes)
