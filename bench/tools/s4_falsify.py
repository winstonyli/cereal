#!/usr/bin/env python3
"""S4 gate (b): is a `target`-typed precondition family-forced, or mis-typed?
(docs/CONFIG_DESIGN.md section 8, S4).

Reads the output of `rename_libuv.py --verbose` (the "S4 ..." lines).  For each refusal whose
list is only `target os = X` or `defined(X)` alternatives it takes the real preprocessor, ignoring the family
table: a marker line `#pragma s4mark` is put before the refused use, the unit is preprocessed
with `cc -E` as it is (the marker must be absent: the group really is off) and with the atom
flipped by -U/-D (the marker should appear).

  confirmed      the marker appears under the flip: the list is right
  refuted        no marker under the flip, and the preprocessor got to the end: mis-typed
  inconclusive   the preprocessor stopped first (a platform header the host lacks)
  header-macro   no marker, but the group tests a macro the host's system headers define:
                 no flag undoes it, so the flip method cannot judge the list (not a failure)
  skipped        the list has a clause that is neither `target os = X` nor `defined(X)`
  baseline-on    the marker is present without the flip: the group was not off

Usage: s4_falsify.py [libuv_output.txt]      (CEREAL_CORPUS: the corpus directory)
Exit status 1 when anything is refuted or baseline-on."""
import os, re, shutil, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import clangd_diff as cd

LINE = re.compile(r"^\s+S4 \w[\w-]*\s+(\S+): checking (\S+): .* at (\S+):(\d+):\d+, "
                  r"which cannot be checked \((.*)\)$")
UNDEF = ["-U__linux__", "-U__linux", "-Ulinux", "-U__gnu_linux__"]
FLIP = {"apple": UNDEF + ["-D__APPLE__", "-D__MACH__"],
        "windows": UNDEF + ["-D_WIN32"],
        "other": UNDEF}


# "os = other" is any one of the systems the family table does not name
OTHER_OS = ["_AIX", "__FreeBSD__", "__OpenBSD__", "__NetBSD__", "__DragonFly__", "__sun",
            "__CYGWIN__", "__MVS__", "__PASE__", "__HAIKU__", "__QNX__", "__GNU__", "__hpux"]


def controlling(lines, line):
    """The idents of the nearest #if/#ifdef/#ifndef above the 1-based line that is still open
    there (the `#else` or `#elif` the use is in belongs to it)."""
    depth = 0
    for i in range(line - 2, -1, -1):
        s = lines[i].strip()
        if s.startswith("#endif"):
            depth += 1
        elif s.startswith("#if"):
            if depth == 0:
                return set(re.findall(r"[A-Za-z_]\w*", s)) - {"if", "ifdef", "ifndef", "defined"}
            depth -= 1
    return set()


def alternatives(lst):
    """[flags, ...] (one -U/-D list per alternative) when the list is `<directive>: to compile
    it: <clause> or ...` with each alternative a `target os = X` or a `defined(X)`; else None."""
    i = lst.find("to compile it: ")
    if i < 0:
        return None
    out = []
    for alt in lst[i + len("to compile it: "):].split(" or "):
        alt = re.sub(r"\s*\(the (?:guard|unit)[^)]*\)", "", alt)
        m = re.fullmatch(r"target `os = (\w+)`", alt)
        d = re.fullmatch(r"`defined\((\w+)\)`", alt)
        if m and m.group(1) in FLIP:
            if m.group(1) == "other":
                out += [FLIP["other"] + ["-D" + o] for o in OTHER_OS]
            else:
                out.append(FLIP[m.group(1)])
        elif d:
            out.append(["-D" + d.group(1)])
        else:
            return None
    return out


def preprocess(root, flags, unit, extra):
    """cc -E; a header the host lacks (another platform's) is stubbed empty, which a marker
    before the use does not care about."""
    stub = os.path.join(root, "_stub")
    for _ in range(60):
        p = subprocess.run(["cc"] + flags + extra + ["-I" + stub, "-E", unit], cwd=root,
                           stdin=subprocess.DEVNULL, capture_output=True, text=True,
                           errors="replace")
        m = re.search(r"fatal error: (\S+): No such file", p.stderr)
        if not (p.returncode and m):
            break
        path = os.path.join(stub, m.group(1))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "w").close()
    return p.returncode, p.stdout


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/s4_libuv.txt")
    cdir = os.environ.get("CEREAL_CORPUS", os.path.expanduser("~/corpus"))
    rel, flags, globs, _q = [p for p in cd.PROJECTS if "libuv" in p[0]][0]
    root = os.path.join(tempfile.mkdtemp(prefix="s4_falsify", dir=os.path.expanduser("~")), "p")
    shutil.copytree(os.path.join(cdir, rel), root, symlinks=True)
    units = cd.proj_units(root, globs)
    tally = {}
    try:
        for ln in open(src, errors="replace"):
            m = LINE.match(ln.rstrip("\n"))
            if not m:
                continue
            name, base, path, line, lst = m.groups()
            path = path.split("/p/", 1)[-1]
            alts = alternatives(lst)
            if alts is None:
                tally["skipped"] = tally.get("skipped", 0) + 1
                continue
            unit = [u for u in units if os.path.basename(u) == base]
            if len(unit) != 1:
                tally["inconclusive"] = tally.get("inconclusive", 0) + 1
                print("  ? %s: unit %s matches %d units" % (name, base, len(unit)))
                continue
            f = os.path.join(root, path)
            orig = open(f, errors="surrogateescape").read().split("\n")
            marked = orig[:int(line) - 1] + ["#pragma s4mark"] + orig[int(line) - 1:]
            open(f, "w", errors="surrogateescape").write("\n".join(marked))
            try:
                rc, out = preprocess(root, flags, unit[0], [])
                if "s4mark" in out:
                    verdict = "baseline-on"
                else:
                    verdict = "refuted"
                    for extra in alts:
                        rc, out = preprocess(root, flags, unit[0], extra)
                        if "s4mark" in out:
                            verdict = "confirmed"
                            break
                        if rc:
                            verdict = "inconclusive"
                    if verdict == "refuted":
                        # a macro the host's system headers define cannot be undone by a flag:
                        # the flip method cannot reach it (O_CLOEXEC under "windows")
                        dm = subprocess.run(["cc"] + flags + ["-I" + os.path.join(root, "_stub"),
                                             "-E", "-dM", unit[0]], cwd=root,
                                            stdin=subprocess.DEVNULL, capture_output=True,
                                            text=True, errors="replace").stdout
                        sysdef = {x.split()[1].split("(")[0] for x in dm.split("\n")
                                  if x.startswith("#define ")}
                        text = "\n".join(orig)
                        if any(i in sysdef and not re.search(r"^\s*#\s*define\s+%s\b" % i, text, re.M)
                               for i in controlling(orig, int(line))):
                            verdict = "header-macro"
            finally:
                open(f, "w", errors="surrogateescape").write("\n".join(orig))
            tally[verdict] = tally.get(verdict, 0) + 1
            if verdict not in ("confirmed", "skipped"):
                print("  %s %s: %s at %s:%s" % (verdict, name, lst, path, line))
    finally:
        shutil.rmtree(os.path.dirname(root), ignore_errors=True)
    for k in sorted(tally):
        print("%-13s %3d" % (k, tally[k]))
    return 1 if tally.get("refuted") or tally.get("baseline-on") else 0


if __name__ == "__main__":
    sys.exit(main())
