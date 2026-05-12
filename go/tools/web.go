package tools

import (
	"context"
	"encoding/json"
	"fmt"
	"net/url"
	"strings"
)

func makeWebSearchTool(apiKey, engineID, endpoint string) Tool {
	return Tool{
		Name:        "web_search",
		Description: "Search the web. Returns up to 10 results with titles, snippets, and URLs. By default uses the DuckDuckGo Instant Answer API (no key required). To use Google Custom Search, set SEARCH_API_KEY + SEARCH_ENGINE_ID. For a custom endpoint, set SEARCH_ENDPOINT with a {query} placeholder.",
		Permission:  PermissionReadOnly,
		TimeoutSec:  15,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"query": map[string]any{
					"type":        "string",
					"description": "Search query (max 500 characters)",
				},
			},
			"required": []string{"query"},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			query, _ := args["query"].(string)
			if query == "" {
				return "", &ToolError{Message: "query is required"}
			}
			if len(query) > 500 {
				query = query[:500]
			}

			useGoogle := apiKey != "" && engineID != ""
			useCustom := endpoint != ""

			var requestURL string
			if useCustom {
				requestURL = strings.Replace(endpoint, "{query}", url.QueryEscape(query), -1)
			} else if useGoogle {
				requestURL = fmt.Sprintf("https://www.googleapis.com/customsearch/v1?key=%s&cx=%s&q=%s",
					url.QueryEscape(apiKey), url.QueryEscape(engineID), url.QueryEscape(query))
			} else {
				// DuckDuckGo Instant Answer API
				rateLimitDDG()
				requestURL = fmt.Sprintf("https://api.duckduckgo.com/?q=%s&format=json&no_html=1&skip_disambig=1",
					url.QueryEscape(query))

				// Retry loop for DDG rate limits
				var lastErr error
				for attempt := 0; attempt < 3; attempt++ {
					body, statusCode, err := httpGet(ctx, requestURL, 15)
					if err != nil {
						lastErr = err
						if attempt < 2 {
							continue
						}
						return "", &ToolError{Message: fmt.Sprintf("web_search error: %s", err)}
					}
					if statusCode == 429 {
						lastErr = fmt.Errorf("HTTP 429 rate limited")
						if attempt < 2 {
							continue
						}
						return "", &ToolError{Message: "web_search: rate limited after retries"}
					}
					if statusCode != 200 {
						return "", &ToolError{Message: fmt.Sprintf("web_search HTTP %d", statusCode)}
					}

					// Parse DDG response
					return formatDDGResponse(body), nil
				}
				return "", &ToolError{Message: fmt.Sprintf("web_search: %s", lastErr)}
			}

			// Google CSE or custom endpoint
			body, statusCode, err := httpGet(ctx, requestURL, 15)
			if err != nil {
				return "", &ToolError{Message: fmt.Sprintf("web_search error: %s", err)}
			}
			if statusCode != 200 {
				return "", &ToolError{Message: fmt.Sprintf("web_search HTTP %d: %s", statusCode, truncateMsg(body, 500))}
			}

			var result map[string]any
			if err := json.Unmarshal([]byte(body), &result); err != nil {
				return "", &ToolError{Message: fmt.Sprintf("web_search JSON parse error: %s", err)}
			}

			return formatGoogleResponse(result), nil
		},
	}
}

func makeWebFetchTool() Tool {
	return Tool{
		Name:        "web_fetch",
		Description: "Fetch the content of a URL. Returns the response body as text. Max 100,000 characters. Use this to read documentation, API references, or web pages. Only returns the raw response body; for search results use web_search.",
		Permission:  PermissionReadOnly,
		TimeoutSec:  15,
		Parameters: map[string]any{
			"type": "object",
			"properties": map[string]any{
				"url": map[string]any{
					"type":        "string",
					"description": "URL to fetch (http/https). Results are cached per session — re-fetching the same URL returns the cached content.",
				},
			},
			"required": []string{"url"},
		},
		Execute: func(ctx context.Context, args map[string]any) (string, error) {
			urlStr, _ := args["url"].(string)
			if urlStr == "" {
				return "", &ToolError{Message: "url is required"}
			}
			if !isValidFetchScheme(urlStr) {
				return "", &ToolError{Message: "web_fetch: only http and https URLs are supported"}
			}

			// Check cache
			fetchCacheLock.Lock()
			if cached, ok := fetchCache[urlStr]; ok {
				fetchCacheLock.Unlock()
				return cached, nil
			}
			fetchCacheLock.Unlock()

			body, statusCode, err := httpGet(ctx, urlStr, 15)
			if err != nil {
				return "", &ToolError{Message: fmt.Sprintf("web_fetch error: %s", err)}
			}
			if statusCode != 200 {
				return "", &ToolError{Message: fmt.Sprintf("web_fetch HTTP %d: %s", statusCode, truncateMsg(body, 500))}
			}

			// Truncate to 100K chars
			if len(body) > 100000 {
				body = body[:100000] + "\n...(truncated, >100000 chars)"
			}

			// Cache the result
			fetchCacheLock.Lock()
			fetchCache[urlStr] = body
			fetchCacheLock.Unlock()

			return body, nil
		},
	}
}

// formatDDGResponse formats a DuckDuckGo Instant Answer API response.
func formatDDGResponse(body string) string {
	var result map[string]any
	if err := json.Unmarshal([]byte(body), &result); err != nil {
		return "(no results found)"
	}

	var sb strings.Builder

	// Direct answer
	if answer, ok := result["Answer"].(string); ok && answer != "" {
		sb.WriteString("Answer: " + answer + "\n\n")
	}

	// Abstract/summary
	if abstract, ok := result["AbstractText"].(string); ok && abstract != "" {
		sb.WriteString(abstract + "\n")
		if src, ok := result["AbstractSource"].(string); ok && src != "" {
			if srcURL, ok := result["AbstractURL"].(string); ok && srcURL != "" {
				sb.WriteString("Source: " + src + " (" + srcURL + ")\n")
			}
		}
		sb.WriteString("\n")
	}

	// Definition
	if def, ok := result["Definition"].(string); ok && def != "" {
		sb.WriteString("Definition: " + def + "\n\n")
	}

	// Heading
	if heading, ok := result["Heading"].(string); ok && heading != "" {
		if _, hasAbstract := result["AbstractText"]; !hasAbstract {
			sb.WriteString("Topic: " + heading + "\n\n")
		}
	}

	// Results
	if results, ok := result["Results"].([]any); ok {
		for _, item := range results {
			if itemMap, ok := item.(map[string]any); ok {
				if text, ok := itemMap["Text"].(string); ok && text != "" {
					sb.WriteString("- " + text + "\n")
					if link, ok := itemMap["FirstURL"].(string); ok && link != "" {
						sb.WriteString("  " + link + "\n")
					}
					sb.WriteString("\n")
				}
			}
		}
	}

	// RelatedTopics
	if topics, ok := result["RelatedTopics"].([]any); ok {
		for _, topic := range topics {
			if topicMap, ok := topic.(map[string]any); ok {
				if subTopics, ok := topicMap["Topics"].([]any); ok {
					// Nested subcategory
					for _, sub := range subTopics {
						if subMap, ok := sub.(map[string]any); ok {
							if text, ok := subMap["Text"].(string); ok && text != "" {
								sb.WriteString("- " + text + "\n")
								if link, ok := subMap["FirstURL"].(string); ok && link != "" {
									sb.WriteString("  " + link + "\n")
								}
								sb.WriteString("\n")
							}
						}
					}
				} else {
					if text, ok := topicMap["Text"].(string); ok && text != "" {
						sb.WriteString("- " + text + "\n")
						if link, ok := topicMap["FirstURL"].(string); ok && link != "" {
							sb.WriteString("  " + link + "\n")
						}
						sb.WriteString("\n")
					}
				}
			}
		}
	}

	if sb.Len() == 0 {
		return "(no results found)"
	}
	return sb.String()
}

// formatGoogleResponse formats a Google CSE or custom endpoint response.
func formatGoogleResponse(data map[string]any) string {
	items, ok := data["items"].([]any)
	if !ok || len(items) == 0 {
		return "(no results found)"
	}

	var sb strings.Builder
	for rank, item := range items {
		if rank >= 10 {
			break
		}
		itemMap, ok := item.(map[string]any)
		if !ok {
			continue
		}

		title, _ := itemMap["title"].(string)
		snippet, _ := itemMap["snippet"].(string)
		link, _ := itemMap["link"].(string)

		if title == "" {
			title = "(no title)"
		}

		sb.WriteString(fmt.Sprintf("%d. %s\n", rank+1, title))
		if snippet != "" {
			sb.WriteString("   " + snippet + "\n")
		}
		if link != "" {
			sb.WriteString("   " + link + "\n")
		}
		sb.WriteString("\n")
	}

	return sb.String()
}

// truncateMsg truncates a string to maxLen chars for error messages.
func truncateMsg(s string, maxLen int) string {
	if len(s) <= maxLen {
		return s
	}
	return s[:maxLen] + "..."
}
