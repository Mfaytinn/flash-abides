# Third-party notices for the standalone engine

The engine is original C++ code, licensed under this repository's `LICENSE` (MIT).

## ABIDES

- **What:** the engine reimplements the semantics of ABIDES, the Agent-Based Interactive
  Discrete Event Simulation from J.P. Morgan Chase
  (https://github.com/jpmorganchase/abides-jpmc-public), as pinned by the organizers' Track 3
  fork. Output must match that fork's event for event.
- **Licence:** BSD-3-Clause, Copyright (c) 2021, J.P. Morgan Chase. The full notice is below.
- **Reference source:** ABIDES Python sources are not included in this checkout or compiled
  into the engine.
- **The organizers' fork:** its additions are licensed MIT, according to the organizers' notice
  in the organizer's original public repository.

### ABIDES BSD-3-Clause notice

```text
BSD 3-Clause License

Copyright (c) 2021, J.P. Morgan Chase
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Example scenario

`examples/scenario.json` is the public Track 3 unit `t3-as01-base-mix` from the Agenthon 2026
organizers' repository, copied unchanged. The organizers license their scenarios under the
MIT License.

## Statically linked runtime libraries

The `simulate` binary in the submission image is linked statically. It contains parts of:

- the GNU C Library, under LGPL-2.1-or-later;
- the GNU C++ runtime (libstdc++ and libgcc), under GPL-3.0 with the GCC Runtime Library
  Exception.

Their sources are available from the GNU project.
