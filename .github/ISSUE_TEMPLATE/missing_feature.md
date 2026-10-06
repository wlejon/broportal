---
name: Missing portal or option
about: an impl.portal interface, method, option or result broportal does not serve
labels: enhancement
---

**What is missing:** the interface and member (or option / result key), with
a link to its section of the xdg-desktop-portal documentation.

**Who calls it:** the application or toolkit that needs it, and what happens
today (an error, a missing key, the frontend picking another backend).

**What the host has to provide:** whether this needs a new callback into the
compositor (a dialog, a capture, a grab) or is answerable by the backend alone.

**What you are building:** what you need it for. A real application is the
most useful answer; it is what decides which gaps get closed first.
