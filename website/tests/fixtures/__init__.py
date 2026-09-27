"""Test fixtures for the generated-site validation/browser workstream.

Contains:
  * sample_site/   - a small valid rendered site (base path "/") used to prove
                     ../contract/test_generated_site_validation.py end to end.
  * sample_site_bioseqdb/ - the same contract rendered below ``/bioseqdb/`` for
                     ../integration/test_browser.py.
  * bad/           - isolated malformed/leaky/duplicate-id/etc. pages, each valid
                     except for one defect, used to prove each validator rule.

The loopback server lives in ``../support/static_fixture_server.py``.
"""
