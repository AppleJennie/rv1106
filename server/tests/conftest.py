"""pytest 共享夹具：httpx ASGI transport（不起真实端口）+ 临时 SQLite 文件。

py3.8 环境下未安装 pytest-asyncio，这里用 pytest_pyfunc_call 钩子直接
asyncio.run 异步用例，避免额外依赖。
"""
import asyncio
import inspect
import os
import tempfile

import httpx
import pytest

# 模块级 app = create_app() 在 import 时会初始化默认库；
# 测试进程统一指向临时文件，避免污染 server/data/。
_DEFAULT_TEST_DB = os.path.join(tempfile.gettempdir(), "dms_server_conftest_default.db")
if os.path.exists(_DEFAULT_TEST_DB):
    os.remove(_DEFAULT_TEST_DB)
os.environ.setdefault("DMS_SERVER_DB", _DEFAULT_TEST_DB)

from app.main import create_app  # noqa: E402


def pytest_pyfunc_call(pyfuncitem):
    """无 pytest-asyncio 时的最小异步用例支持。"""
    func = pyfuncitem.obj
    if inspect.iscoroutinefunction(func):
        kwargs = {
            name: pyfuncitem.funcargs[name]
            for name in pyfuncitem._fixtureinfo.argnames
        }
        asyncio.run(func(**kwargs))
        return True
    return None


@pytest.fixture()
def client(tmp_path, monkeypatch):
    """每个用例独立的 SQLite 文件 + ASGI 直连客户端。"""
    db_file = tmp_path / "test.db"
    monkeypatch.setenv("DMS_SERVER_DB", str(db_file))
    app = create_app()
    transport = httpx.ASGITransport(app=app)
    return httpx.AsyncClient(transport=transport, base_url="http://test")
