# Stage 1: Install the MCP server
FROM node:22-slim AS installer

RUN npm install -g @modelcontextprotocol/server-filesystem@0.6.2

# Stage 2: Final image
FROM node:22-slim

# Copy the pre-installed package
COPY --from=installer /usr/local/lib/node_modules /usr/local/lib/node_modules
COPY --from=installer /usr/local/bin /usr/local/bin

# Create symlinks so npx finds the package
RUN ln -sf /usr/local/lib/node_modules/.package-lock.json /usr/local/ 2>/dev/null; \
    mkdir -p /usr/local/lib/node_modules/.package-lock.json 2>/dev/null; true

# Create the test file tree
RUN mkdir -p /tmp/test-files/projects /tmp/test-files/documents /tmp/test-files/data

# ── Sample files ──
RUN echo "# Project Alpha\n\n## Overview\nThis is a sample project for MCP testing.\n\n## Structure\n- src/\n- tests/\n- docs/" > /tmp/test-files/projects/README.md

RUN echo "name: alpha\nversion: 1.0.0\ndescription: Sample project" > /tmp/test-files/projects/alpha.yaml

RUN echo '{\n  "project": "alpha",\n  "version": "1.0.0",\n  "dependencies": {\n    "libfoo": "^2.0"\n  }\n}' > /tmp/test-files/projects/alpha.json

RUN echo "Hello, World!" > /tmp/test-files/documents/hello.txt

RUN echo "Line 1\nLine 2\nLine 3\nLine 4\nLine 5" > /tmp/test-files/documents/multiline.txt

RUN echo "const greet = (name) => {\n  return \\`Hello \\${name}!\\`;\n};\n\nmodule.exports = { greet };" > /tmp/test-files/projects/greet.js

# Keep the container alive
CMD ["sleep", "infinity"]
