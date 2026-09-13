# Sphinx configuration for the ShapoGFX reference manual.
# https://www.sphinx-doc.org/en/master/usage/configuration.html

project = "ShapoGFX"
copyright = "2026, Shapoco"
author = "Shapoco"
language = "ja"

extensions = []

templates_path = ["_templates"]
exclude_patterns = ["_build", "Thumbs.db", ".DS_Store", ".gitignore"]

# -- HTML output -------------------------------------------------------------

html_theme = "sphinx_rtd_theme"
html_static_path = ["_static"]
html_css_files = ["custom.css"]
html_title = "ShapoGFX Reference Manual"
html_short_title = "ShapoGFX"
html_show_sourcelink = False
html_copy_source = False

html_theme_options = {
    "navigation_depth": 3,
    "collapse_navigation": False,
    "prev_next_buttons_location": "both",
}

# "Edit on GitHub" link at the top of every page
html_context = {
    "display_github": True,
    "github_user": "shapoco",
    "github_repo": "shapo-gfx",
    "github_version": "main",
    "conf_py_path": "/docsrc/",
}

# Default highlighting language of code blocks
highlight_language = "cpp"
