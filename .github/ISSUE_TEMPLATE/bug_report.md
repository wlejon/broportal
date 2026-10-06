---
name: Bug report
about: broportal answers a portal call wrongly, rejects a valid one, mis-encodes a reply or signal, hangs, or crashes
labels: bug
---

**The call** (interface, method and arguments; a `gdbus call` or `busctl call`
line is ideal):

```
```

**What xdg-desktop-portal or the application expected** (the spec section, the
frontend's log with `--verbose`, or the application's error):

```
```

**What broportal answered instead** (the reply or error, `dbus-monitor` output
if it is a signal; paste it):

```
```

**Does it reproduce in the tests?** Which `ctest` test fails, if any:

**Environment:**
- Distribution and version, xdg-desktop-portal version:
- The compositor or host embedding broportal, and which callbacks it sets:
- broportal commit:
