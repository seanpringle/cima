#!/usr/bin/env bash
set -uo pipefail

HOST="${HOST:-127.0.0.1:11000}"
MODEL="${MODEL:-gpt-3.5-turbo}"
SYSTEM_PROMPT="${SYSTEM_PROMPT:-You are a helpful assistant.}"
SAFE_DIR="${SAFE_DIR:-$(pwd)}"
URL="http://${HOST}/v1/chat/completions"

command -v jq &>/dev/null || {
    echo "Error: jq is required. Install with: sudo apt install jq (or brew install jq)" >&2
    exit 1
}

TOOLS='[
    {
        "type": "function",
        "function": {
            "name": "list_files",
            "description": "List files and directories in a given path",
            "parameters": {
                "type": "object",
                "properties": {
                    "path": {
                        "type": "string",
                        "description": "Directory path to list"
                    }
                },
                "required": ["path"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "read_file",
            "description": "Read the contents of a file (max 200 lines)",
            "parameters": {
                "type": "object",
                "properties": {
                    "path": {
                        "type": "string",
                        "description": "Path to the file to read"
                    }
                },
                "required": ["path"]
            }
        }
    }
]'

messages=$(jq -nc --arg content "$SYSTEM_PROMPT" '[{"role": "system", "content": $content}]')

chat() {
    local line data token payload
    local has_tool_calls tool_name tool_id tool_args
    local asst_msg tool_msg result tool_path

    while true; do
        full_response=""
        has_tool_calls=false
        tool_name=""; tool_id=""; tool_args=""

        payload=$(jq -nc \
            --arg model "$MODEL" \
            --argjson msgs "$messages" \
            --argjson tools "$TOOLS" \
            '{
                model: $model,
                messages: $msgs,
                tools: $tools,
                stream: true
            }')

        while IFS= read -r line; do
            [[ "$line" != data:* ]] && continue
            data="${line#data: }"
            data="${data%$'\r'}"
            [[ "$data" == "[DONE]" ]] && break

            if echo "$data" | jq -e '.choices[0].delta.tool_calls' >/dev/null 2>&1; then
                has_tool_calls=true
                n=$(echo "$data" | jq -r '.choices[0].delta.tool_calls[0].function.name // ""')
                id=$(echo "$data" | jq -r '.choices[0].delta.tool_calls[0].id // ""')
                a=$(echo "$data" | jq -r '.choices[0].delta.tool_calls[0].function.arguments // ""')
                [[ -n "$n" ]] && tool_name="$n"
                [[ -n "$id" ]] && tool_id="$id"
                tool_args+="$a"
            else
                token=$(echo "$data" | jq -r '.choices[0].delta.content // ""')
                if [[ -n "$token" ]]; then
                    printf "%s" "$token"
                    full_response+="$token"
                fi
            fi
        done < <(curl -N -s "$URL" -H "Content-Type: application/json" -d "$payload" 2>/dev/null || true)

        if [[ "$has_tool_calls" == "true" ]]; then
            asst_msg=$(jq -nc \
                --arg id "$tool_id" \
                --arg name "$tool_name" \
                --arg args "$tool_args" \
                '{
                    "role": "assistant",
                    "content": null,
                    "tool_calls": [{
                        "id": $id,
                        "type": "function",
                        "function": {
                            "name": $name,
                            "arguments": $args
                        }
                    }]
                }')
            messages=$(echo "$messages" | jq -c --argjson msg "$asst_msg" '. + [$msg]')

            result=""
            tool_path=$(echo "$tool_args" | jq -r '.path // ""' 2>/dev/null) || tool_path=""

            echo "→ ${tool_name}(\"${tool_path}\")" >&2

            if [[ -z "$tool_path" ]]; then
                result="Error: could not parse tool arguments"
            elif [[ -n "$SAFE_DIR" && "$tool_path" != "$SAFE_DIR"* ]]; then
                result="Error: path must be under ${SAFE_DIR} (got: ${tool_path})"
            fi

            if [[ -z "$result" ]]; then
                case "$tool_name" in
                    list_files) result=$(ls -la "$tool_path" 2>&1) ;;
                    read_file) result=$(head -200 "$tool_path" 2>&1) ;;
                    *) result="Error: unknown tool '${tool_name}'" ;;
                esac
            fi

            tool_msg=$(jq -nc \
                --arg id "$tool_id" \
                --arg content "$result" \
                '{"role": "tool", "tool_call_id": $id, "content": $content}')
            messages=$(echo "$messages" | jq -c --argjson msg "$tool_msg" '. + [$msg]')

            result=""
            continue
        fi

        echo
        if [[ -z "$full_response" ]]; then
            echo "(no response — is llama.cpp running on ${HOST}?)" >&2
            return 1
        fi
        messages=$(echo "$messages" | jq -c --arg content "$full_response" '. + [{"role": "assistant", "content": $content}]')
        break
    done
}

echo "llm-chat — ${URL}  model: ${MODEL}  tools: list_files, read_file"
echo "/exit  /clear  /model <name>"
while IFS= read -e -r -p "> " input; do
    case "$input" in
        /exit|/quit) break ;;
        /clear) messages=$(jq -nc --arg content "$SYSTEM_PROMPT" '[{"role": "system", "content": $content}]'); echo "cleared"; continue ;;
        /model*)
            rest="${input#/model }"
            if [[ "$rest" != "$input" ]]; then
                MODEL="$rest"
                echo "model: ${MODEL}"
            else
                echo "usage: /model <name>"
            fi
            continue
            ;;
        "") continue ;;
    esac
    messages=$(echo "$messages" | jq -c --arg content "$input" '. + [{"role": "user", "content": $content}]')
    chat
done
echo "bye!"
