"""Test fixtures for the generated-site validation/browser workstream.

Contains:
  * server.py      - loopback static fixture server that applies the site's own
                     security-headers.txt to every response and mounts a site at
                     its configured base path (used by ../browser_test.py).
  * sample_site/   - a small valid rendered site (base path "/") used to prove
                     ../validate_site.py end to end.
  * bad/           - isolated malformed/leaky/duplicate-id/etc. pages, each valid
                     except for one defect, used to prove each validator rule.
"""
