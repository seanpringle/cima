#!/usr/bin/env bash
set -euo pipefail

# Helper script to manage a Docker-based MCP filesystem server for E2E testing.
# Builds an image with Node.js + @modelcontextprotocol/server-filesystem
# and a sample file tree, then runs it as a background container.

IMAGE_NAME="cima-mcp-test"
CONTAINER_NAME="cima-mcp-test"

case "${1:-build}" in
  build)
    echo "==> Building Docker image..."
    docker build -t "$IMAGE_NAME" -f "$(dirname "$0")/Dockerfile" "$(dirname "$0")"
    echo "==> Done. Image '$IMAGE_NAME' built."
    ;;

  start)
    echo "==> Starting container..."
    docker run -d --name "$CONTAINER_NAME" "$IMAGE_NAME" sleep infinity
    echo "==> Container '$CONTAINER_NAME' running."
    ;;

  stop)
    echo "==> Stopping container..."
    docker rm -f "$CONTAINER_NAME" 2>/dev/null || true
    echo "==> Container removed."
    ;;

  restart)
    "$0" stop
    "$0" start
    ;;

  shell)
    echo "==> Opening shell in container..."
    docker exec -it "$CONTAINER_NAME" /bin/bash
    ;;

  test)
    echo "==> Quick connectivity test (initialize + tools/list)..."
    printf '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"cima-test","version":"1.0"}}}\n{"jsonrpc":"2.0","method":"notifications/initialized","params":{}}\n{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}\n' | \
      docker exec -i "$CONTAINER_NAME" \
        npx -y @modelcontextprotocol/server-filesystem /tmp/test-files | \
      head -c 1000
    echo ""
    echo "==> If you see JSON-RPC responses above, the MCP server works."
    ;;

  *)
    echo "Usage: $0 {build|start|stop|restart|shell|test}"
    exit 1
    ;;
esac
