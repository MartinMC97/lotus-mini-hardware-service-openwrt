# Third-party sources

`picojson/picojson.h` is picojson **v1.3.0** by Cybozu Labs and Kazuho Oku,
under the BSD 2-clause license reproduced in the header.

Source: https://github.com/kazuho/picojson/tree/v1.3.0

Original header SHA-256:
`5ddf7276d04926da7be243e7af49258e78cc27278ee9097ba45b942c7a6b5f9d`.

Local change: zero-initialize the union in the null constructor to avoid copying
uninitialized union storage (reported by GCC 14 with strict warnings).
Trailing whitespace has been mechanically removed; license text is retained.
Strict UTF-8, nesting limits, duplicate-key rejection and full-input validation
are implemented in our wrapper, not by modifying the parser.

No audio prompts or code from either assistant have been copied into this phase.
