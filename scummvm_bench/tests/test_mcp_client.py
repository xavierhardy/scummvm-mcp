"""Pure helpers of the vendored MCP client (no sockets)."""

from scummvm_bench.mcp_client import McpClient, McpError


def test_extract_result_unwraps_text_content() -> None:
    data = {
        "result": {
            "content": [{"type": "text", "text": '{"room": {"id": 55}}'}],
        }
    }
    assert McpClient._extract_result(data) == {"room": {"id": 55}}


def test_extract_result_plain_result() -> None:
    data = {"result": {"room": {"id": 7}}}
    assert McpClient._extract_result(data) == {"room": {"id": 7}}


def test_extract_result_missing() -> None:
    assert McpClient._extract_result({}) == {}


def test_error_from_dict_carries_code() -> None:
    err = McpClient._error_from({"message": "bad params", "code": -32602}, "act")
    assert isinstance(err, McpError)
    assert err.code == -32602
    assert "bad params" in str(err)


def test_error_from_non_dict() -> None:
    err = McpClient._error_from("boom", "walk")
    assert err.code is None
    assert "boom" in str(err)


def test_mcp_error_defaults() -> None:
    err = McpError("oops")
    assert err.code is None
