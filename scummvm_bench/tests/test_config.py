"""Config loading and derived views."""

from scummvm_bench.config import default_config_path, load_config

TOML = """
scummvm_binary = "/x/scummvm"
save_folder = "/saves"
lms_bin = "/opt/lms"

[[games]]
id = "monkey-ega-demo"
path = "/g/m"
save_slot = 1

[[local_models]]
key = "google/gemma"
provider = "local"
model = "gemma"
context_length = 4096
gpu = "max"
ttl = 60
"""


def test_load_config_parses_games_and_models(tmp_path) -> None:
    path = tmp_path / "bench.toml"
    path.write_text(TOML)
    config = load_config(str(path))

    assert config.scummvm_binary == "/x/scummvm"
    assert config.save_folder == "/saves"
    assert config.lms_bin == "/opt/lms"
    game = config.games["monkey-ega-demo"]
    assert game.game_path == "/g/m"
    assert game.save_slot == 1
    model = config.local_models[0]
    assert model.context_length == 4096
    assert model.gpu == "max"
    assert model.ttl == 60


def test_session_config_and_model_map(tmp_path) -> None:
    path = tmp_path / "bench.toml"
    path.write_text(TOML)
    config = load_config(str(path))

    session = config.session_config()
    assert session.scummvm_binary == "/x/scummvm"
    assert session.save_folder == "/saves"
    # CLI override wins
    assert config.session_config(save_folder="/other").save_folder == "/other"

    mapping = config.local_model_map()
    assert ("local", "gemma") in mapping


def test_load_default_config() -> None:
    config = load_config(default_config_path())
    assert "monkey-ega-demo" in config.games
