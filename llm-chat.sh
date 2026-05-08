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
    },
    {
        "type": "function",
        "function": {
            "name": "grep_files",
            "description": "Search file contents using a regex pattern (max 50 results)",
            "parameters": {
                "type": "object",
                "properties": {
                    "pattern": {
                        "type": "string",
                        "description": "Regex pattern to search for"
                    },
                    "path": {
                        "type": "string",
                        "description": "File or directory to search in (defaults to project root)"
                    }
                },
                "required": ["pattern"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "write_file",
            "description": "Write content to a file, creating parent directories if needed",
            "parameters": {
                "type": "object",
                "properties": {
                    "path": {
                        "type": "string",
                        "description": "File path"
                    },
                    "content": {
                        "type": "string",
                        "description": "Content to write"
                    }
                },
                "required": ["path", "content"]
            }
        }
    },
    {
        "type": "function",
        "function": {
            "name": "run_bash",
            "description": "Run a bash command in the project directory (e.g. build, test, lint). Output is capped at 100 lines.",
            "parameters": {
                "type": "object",
                "properties": {
                    "command": {
                        "type": "string",
                        "description": "Shell command to execute"
                    }
                },
                "required": ["command"]
            }
        }
    }
]'

messages=$(jq -nc --arg content "$SYSTEM_PROMPT" '[{"role": "system", "content": $content}]')

resolve_path() {
    local p="$1"
    if [[ "$p" != /* ]]; then
        p="${SAFE_DIR}/${p}"
    fi
    if command -v realpath &>/dev/null; then
        p=$(realpath -m "$p" 2>/dev/null) || return 1
    fi
    if [[ "$p" != "$SAFE_DIR"* ]]; then
        return 1
    fi
    echo "$p"
}

chat() {
    local line data token payload
    local has_tool_calls tool_name tool_id tool_args
    local asst_msg tool_msg result tool_path
    local raw_path resolved pattern gpath content cmd

    while true; do
        full_response=""
        has_tool_calls=false
        tool_name=""; tool_id=""; tool_args=""
        raw_path=""; resolved=""; pattern=""; gpath=""; content=""; cmd=""

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
            raw_path=$(echo "$tool_args" | jq -r '.path // ""' 2>/dev/null) || raw_path=""

            case "$tool_name" in
                list_files|read_file|write_file)
                    if [[ -z "$raw_path" ]]; then
                        result="Error: path is required"
                    else
                        resolved=$(resolve_path "$raw_path") || result="Error: path must be under ${SAFE_DIR}"
                    fi
                    if [[ "$tool_name" == "write_file" ]]; then
                        content=$(echo "$tool_args" | jq -r '.content // ""' 2>/dev/null) || content=""
                    fi
                    ;;

                grep_files)
                    pattern=$(echo "$tool_args" | jq -r '.pattern // ""' 2>/dev/null) || pattern=""
                    gpath=$(echo "$tool_args" | jq -r '.path // "."' 2>/dev/null) || gpath="."
                    if [[ -z "$pattern" ]]; then
                        result="Error: pattern is required"
                    else
                        resolved=$(resolve_path "$gpath") || result="Error: path must be under ${SAFE_DIR}"
                    fi
                    ;;

                run_bash)
                    cmd=$(echo "$tool_args" | jq -r '.command // ""' 2>/dev/null) || cmd=""
                    if [[ -z "$cmd" ]]; then
                        result="Error: command is required"
                    fi
                    ;;

                *)
                    result="Error: unknown tool '${tool_name}'"
                    ;;
            esac

            if [[ -z "$result" ]]; then
                case "$tool_name" in
                    list_files)
                        echo "→ list_files(\"${resolved}\")" >&2
                        result=$(ls -la "$resolved" 2>&1)
                        ;;
                    read_file)
                        echo "→ read_file(\"${resolved}\")" >&2
                        result=$(head -200 "$resolved" 2>&1)
                        ;;
                    grep_files)
                        echo "→ grep_files(\"${pattern}\")" >&2
                        result=$(cd "$SAFE_DIR" && grep -rn -- "$pattern" "$gpath" 2>&1 | head -50) || true
                        if [[ -z "$result" ]]; then
                            result="(no matches)"
                        fi
                        ;;
                    write_file)
                        echo "→ write_file(\"${resolved}\")" >&2
                        mkdir -p "$(dirname "$resolved")" 2>/dev/null
                        if printf '%s' "$content" > "$resolved" 2>/dev/null; then
                            local wc
                            wc=$(printf '%s' "$content" | wc -c | tr -d ' ')
                            result="ok (${wc} bytes written)"
                        else
                            result="Error: write failed"
                        fi
                        ;;
                    run_bash)
                        echo "→ run_bash: ${cmd}" >&2
                        result=$(cd "$SAFE_DIR" && eval "$cmd" 2>&1 | tail -100) || true
                        if [[ ${#result} -gt 4000 ]]; then
                            result="${result:0:4000}...(truncated, ${#result} total chars)"
                        fi
                        ;;
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

echo "llm-chat — ${URL}  model: ${MODEL}  tools: list_files, read_file, grep_files, write_file, run_bash"
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
