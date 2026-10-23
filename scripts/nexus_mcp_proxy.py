"""Stdio compatibility client; every tool uses the persistent authenticated server."""
from mcp import ClientSession
from mcp.client.streamable_http import streamable_http_client
from mcp.server.fastmcp import FastMCP
from nexus_mcp_control import ENDPOINT, client_token
from typing import Any
import httpx

proxy = FastMCP('Nexus Diagnostics Connector')
annotations = {'readOnlyHint': True, 'destructiveHint': False, 'openWorldHint': False}


async def call(name, arguments=None):
    async with httpx.AsyncClient(headers={'Authorization': 'Bearer ' + client_token()}, trust_env=False, timeout=75, follow_redirects=False) as http:
        async with streamable_http_client(ENDPOINT, http_client=http) as (read, write, _):
            async with ClientSession(read, write) as session:
                await session.initialize()
                result = await session.call_tool(name, arguments or {})
                if result.isError:
                    raise ValueError('Nexus MCP tool failed')
                return result.structuredContent


@proxy.tool(annotations=annotations, structured_output=True)
async def nexus_health() -> dict[str, Any]:
    """Read service, node, worker and memory diagnostics from shared MCP."""
    return await call('nexus_health')


@proxy.tool(annotations=annotations, structured_output=True)
async def nexus_telemetry() -> dict[str, Any]:
    """Read native counters, freshness and latest timings."""
    return await call('nexus_telemetry')


@proxy.tool(annotations=annotations, structured_output=True)
async def nexus_gpu() -> dict[str, Any]:
    """Read authorized GPU measurements."""
    return await call('nexus_gpu')


@proxy.tool(annotations=annotations, structured_output=True)
async def nexus_performance_sample(seconds: int = 10) -> dict[str, Any]:
    """Measure native counter deltas for 2..60 seconds."""
    return await call('nexus_performance_sample', {'seconds': seconds})


@proxy.tool(annotations=annotations, structured_output=True)
async def nexus_api_diagnostics() -> dict[str, Any]:
    """Read optional extended diagnostics if the agent API is enabled."""
    return await call('nexus_api_diagnostics')


@proxy.tool(annotations=annotations, structured_output=True)
async def nexus_read_source(path: str) -> dict[str, Any]:
    """Read a bounded allowlisted tracked source file."""
    return await call('nexus_read_source', {'path': path})


if __name__ == '__main__':
    proxy.run(transport='stdio')
