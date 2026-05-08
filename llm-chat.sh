#!/usr/bin/env bash
set -uo pipefail

_script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$_script_dir/.env" ]]; then
    set -a; source "$_script_dir/.env"; set +a
elif [[ -f .env ]]; then
    set -a; source .env; set +a
fi

API_BASE="${API_BASE:-http://127.0.0.1:11000/v1}"
API_KEY="${API_KEY:-}"
MODEL="${MODEL:-deepseek-v4-flash}"
SYSTEM_PROMPT="${SYSTEM_PROMPT:-You are a helpful assistant.}"
SAFE_DIR="${SAFE_DIR:-$(pwd)}"
URL="${API_BASE}/chat/completions"

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
    local _tmp _raw_debug _nlen2 _gray

    while true; do
        full_response=""
        reasoning=""
        has_tool_calls=false
        tool_name=""; tool_id=""; tool_args=""
        raw_path=""; resolved=""; pattern=""; gpath=""; content=""; cmd=""
        _gray=false

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

        local _tmp _raw_debug=""
        _tmp=$(mktemp)
        curl -N -s "$URL" \
            -H "Content-Type: application/json" \
            ${API_KEY:+-H "Authorization: Bearer ${API_KEY}"} \
            -d "$payload" > "$_tmp" 2>/dev/null || true

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
                reason=$(echo "$data" | jq -r '.choices[0].delta.reasoning_content // ""')
                if [[ -n "$reason" ]]; then
                    if [[ "$_gray" == false ]]; then
                        printf "\033[90m"
                        _gray=true
                    fi
                    printf "%s" "$reason"
                    reasoning+="$reason"
                fi
                token=$(echo "$data" | jq -r '.choices[0].delta.content // ""')
                if [[ -z "$token" ]]; then
                    _nlen2=$(echo "$data" | jq -r '.choices[0].delta.content // "" | length' 2>/dev/null || echo 0)
                    if [[ "$_nlen2" -gt 0 ]]; then
                        token=$(printf '\n%.0s' $(seq 1 "$_nlen2") && printf 'x'); token="${token%x}"
                    fi
                fi
                if [[ -n "$token" ]]; then
                    if [[ "$_gray" == true ]]; then
                        printf "\033[0m\n"
                        _gray=false
                    fi
                    printf "%s" "$token"
                    full_response+="$token"
                fi
            fi
        done < "$_tmp"

        if [[ "$_gray" == true ]]; then
            printf "\033[0m"
            [[ -z "$full_response" ]] && printf "\n"
            _gray=false
        fi

        if [[ -z "$full_response" && "$has_tool_calls" != "true" ]]; then
            _raw_debug=$(head -3 "$_tmp" | tr '\n' ';' | head -c 500)
        fi
        rm -f "$_tmp"

        if [[ "$has_tool_calls" == "true" ]]; then
            asst_msg=$(jq -nc \
                --arg id "$tool_id" \
                --arg name "$tool_name" \
                --arg args "$tool_args" \
                --arg reasoning "$reasoning" \
                '{
                    "role": "assistant",
                    "content": null,
                    "reasoning_content": $reasoning,
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
            local _err="${API_BASE}"
            [[ -n "$_raw_debug" ]] && _err+="  raw: ${_raw_debug}"
            echo "(no response — ${_err})" >&2
            return 1
        fi
        if [[ -n "$reasoning" ]]; then
            messages=$(echo "$messages" | jq -c \
                --arg content "$full_response" \
                --arg reasoning "$reasoning" \
                '. + [{"role": "assistant", "content": $content, "reasoning_content": $reasoning}]')
        else
            messages=$(echo "$messages" | jq -c --arg content "$full_response" '. + [{"role": "assistant", "content": $content}]')
        fi
        break
    done
}

auth_info=""
[[ -n "$API_KEY" ]] && auth_info="  [auth: bearer]"
echo "llm-chat — ${URL}  model: ${MODEL}  tools: list_files, read_file, grep_files, write_file, run_bash${auth_info}"
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
