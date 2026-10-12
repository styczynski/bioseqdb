#!/usr/bin/env python3
"""SVG paint references must survive both generated theme palettes."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from theme_palette import theme_svg, tokenize_colors, variables_for


class SvgPaintReferences(unittest.TestCase):
    def test_fragment_urls_are_not_colors(self):
        css = 'fill:url(#fade-x);stroke:url("#abc");color:#46b8de'
        themed = tokenize_colors(css)
        self.assertIn('fill:url(#fade-x)', themed)
        self.assertIn('stroke:url("#abc")', themed)
        self.assertIn('color:var(--theme-46b8de, #46b8de)', themed)
        self.assertEqual(set(variables_for(themed)), {'--theme-46b8de'})

    def test_svg_mask_and_gradient_references_survive_regeneration(self):
        svg = '<svg><defs><linearGradient id="fade-x"><stop stop-color="#ffffff"/></linearGradient></defs><rect fill="url(#fade-x)" stroke="#46b8de"/></svg>'
        themed = theme_svg(svg)
        self.assertIn('fill="url(#fade-x)"', themed)
        self.assertNotIn('--theme-ffaadd', themed)
        self.assertEqual(theme_svg(themed), themed)


if __name__ == '__main__':
    unittest.main()
