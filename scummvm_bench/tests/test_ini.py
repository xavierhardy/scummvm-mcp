"""Rendering per-session scummvm.ini files from templates."""

from scummvm_bench.ini import default_template_path, render_ini


def test_default_template_path() -> None:
    path = default_template_path("monkey-ega-demo")
    assert path.endswith("ini_templates/scummvm_monkey-ega-demo.ini")


def test_render_ini_substitutes_placeholders(tmp_path) -> None:
    template = default_template_path("monkey-ega-demo")
    dest = tmp_path / "scummvm.ini"
    logfile = tmp_path / "engine.log"
    result = render_ini(template, "/games/monkey", 23456, str(logfile), str(dest))

    assert result == str(dest)
    content = dest.read_text()
    assert "/games/monkey" in content
    assert "mcp_port=23456" in content
    assert str(logfile) in content
    # no unrendered placeholders remain
    assert "%(" not in content
