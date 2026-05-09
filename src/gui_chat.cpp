#include "gui_chat.h"

#include "imgui.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>

extern std::atomic<bool> g_interrupted;

void render_content(ImFont* normal_font, ImFont* bold_font, const std::string& text) {
  using namespace ImGui;
  using std::string;
  using std::string_view;
  using std::vector;

  string copy = text;
  copy.erase(std::remove_if(copy.begin(), copy.end(), [](auto c) { return c == '\r'; }), copy.end());

  string_view src(copy);

  auto canvas = GetContentRegionAvail();

  auto parseText = [&](string_view txt) {
    bool bold = false;
    bool code = false;
    int colors = 0;

    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 200, 200, 255));
    colors++;

    std::stringstream fragment;

    auto flush = [&]() {
      string str = fragment.str();
      fragment.str("");

      string_view prev(str);

      while (prev.size()) {
        auto next = prev;
        while (next.size() && !isspace(next.front())) next.remove_prefix(1);
        if (next.size() && isspace(next.front())) next.remove_prefix(1);
        string part(string_view(prev.data(), next.data()-prev.data()));
        auto size = CalcTextSize(part.c_str());
        if (GetCursorPos().x + size.x >= canvas.x) NewLine();
        auto at = GetCursorPos();
        TextUnformatted(part.c_str());
        SetCursorPos(ImVec2(at.x + size.x, at.y));
        prev = next;
      }
    };

    while (txt.size()) {
      if (txt.starts_with("**") && !bold) {
        flush();
        txt.remove_prefix(2);
        bold = true;
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
        colors++;
        continue;
      }
      if (txt.starts_with("**") && bold) {
        flush();
        txt.remove_prefix(2);
        bold = false;
        PopStyleColor();
        colors--;
        continue;
      }

      if (txt.starts_with("`") && !code) {
        flush();
        txt.remove_prefix(1);
        code = true;
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 200, 255, 255));
        colors++;
        continue;
      }
      if (txt.starts_with("`") && code) {
        flush();
        txt.remove_prefix(1);
        code = false;
        PopStyleColor();
        colors--;
        continue;
      }

      fragment << txt.front();
      txt.remove_prefix(1);
    }

    flush();
    PopStyleColor(colors);
  };

  auto scanLine = [&]() {
    string_view cur = src;
    while (cur.size() && cur.front() != '\n') cur.remove_prefix(1);
    string_view line(src.data(), cur.data()-src.data());
    if (cur.size() && cur.front() == '\n') cur.remove_prefix(1);
    src = cur;
    return line;
  };

  auto endBlock = [&]() {
    while (src.starts_with("\n")) {
      src.remove_prefix(1);
    }
    NewLine();
  };

  auto parseCodeBlock = [&]() {
    scanLine(); // ```
    auto drawList = GetWindowDrawList();
    ImDrawListSplitter splitter;
    splitter.Split(drawList, 2);
    splitter.SetCurrentChannel(drawList, 1);
    auto tl = GetCursorScreenPos();
    auto size = GetContentRegionAvail();
    Indent();
    NewLine();
    while (src.size()) {
      auto line = scanLine();
      if (line.starts_with("```")) break;
      TextUnformatted(line.data(), line.data() + line.size());
    }
    NewLine();
    Unindent();
    ImVec2 br(GetCursorScreenPos().x + size.x, GetCursorScreenPos().y);
    splitter.SetCurrentChannel(drawList, 0);
    drawList->AddRectFilled(tl, br, GetColorU32(ImGuiCol_FrameBgActive));
    splitter.Merge(drawList);
    endBlock();
  };

  int tables = 0;

  while (src.size()) {
    if (src.front() == '\n') {
      src.remove_prefix(1);
      continue;
    }

    if (src.starts_with("#")) {
      parseText(scanLine());
      endBlock();
      continue;
    }

    if (src.starts_with("```")) {
      parseCodeBlock();
      continue;
    }

    if (src.starts_with("---")) {
      scanLine();
      Separator();
      endBlock();
      continue;
    }

    if (src.starts_with("* ")) {
      while (src.starts_with("* ")) {
        Bullet();
        src.remove_prefix(2);
        parseText(scanLine());
        while (src.starts_with("  ")) {
          NewLine();
          NewLine();
          src.remove_prefix(2);
          parseText(scanLine());
        }
        NewLine();
      }
      endBlock();
      continue;
    }

    if (src.starts_with("- ")) {
      while (src.starts_with("- ")) {
        Bullet();
        src.remove_prefix(2);
        parseText(scanLine());
        while (src.starts_with("  ")) {
          NewLine();
          NewLine();
          src.remove_prefix(2);
          parseText(scanLine());
        }
        NewLine();
      }
      endBlock();
      continue;
    }

    auto numberedItem = [&]() {
      auto cursor = src;
      while (cursor.size() && std::isdigit(cursor.front())) cursor.remove_prefix(1);
      return cursor.data() != src.data() && cursor.size() && cursor.front() == '.';
    };

    if (numberedItem()) {
      Indent(CalcTextSize("_").x);
      while (numberedItem()) {
        parseText(scanLine());
        while (src.starts_with("  ")) {
          NewLine();
          NewLine();
          src.remove_prefix(2);
          parseText(scanLine());
        }
        NewLine();
      }
      endBlock();
      Unindent(CalcTextSize("_").x);
      continue;
    }

    auto tableSeparator = [&]() {
      return src.starts_with("|---") || src.starts_with("| ---");
    };

    auto scanTableCell = [&]() {
      auto left = src;
      while (src.size() && !src.starts_with("|")) src.remove_prefix(1);
      auto right = src;
      string_view cell(left.data(), left.size()-right.size());
      while (cell.starts_with(" ")) cell.remove_prefix(1);
      while (cell.ends_with(" ")) cell.remove_suffix(1);
      return cell;
    };

    if (src.starts_with("|")) {
      vector<string_view> headers;
      while (src.starts_with("|")) {
        src.remove_prefix(1);
        auto header = scanTableCell();
        if (!header.size()) break;
        headers.push_back(header);
      }
      scanLine();

      if (!headers.size()) {
        headers.emplace_back();
      }

      string tableId = "##table-" + std::to_string(++tables);
      BeginTable(tableId.c_str(), headers.size());
      for (auto [i,header]: headers | std::ranges::views::enumerate) {
        string title = string(header) + "##col-" + std::to_string(i);
        TableSetupColumn(title.c_str());
      }

      while (src.starts_with("|")) {
        if (src.starts_with("|---") || src.starts_with("| ---")) {
          scanLine();
          TableHeadersRow();
          continue;
        }

        TableNextRow();
        for (size_t i = 0; src.starts_with("|"); i++) {
          src.remove_prefix(1);
          TableSetColumnIndex(i);
          parseText(scanTableCell());
        }
        scanLine();
      }

      EndTable();
      NewLine();
      continue;
    }

    parseText(scanLine());
    endBlock();
    if (src.size()) NewLine();
  }
}

static void render_code_block(const std::string& text) {
  if (text.empty())
    return;

  int lines = 1;
  for (char c : text)
    if (c == '\n')
      lines++;

  float line_height = ImGui::GetTextLineHeightWithSpacing();
  float height = lines * line_height;

  ImVec2 start_pos = ImGui::GetCursorScreenPos();
  float width = ImGui::GetContentRegionAvail().x;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(ImVec2(start_pos.x, start_pos.y), ImVec2(start_pos.x + width, start_pos.y + height + 8), IM_COL32(30, 30, 40, 255));

  ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 200, 200, 255));
  ImGui::SetCursorScreenPos(ImVec2(start_pos.x + 8, start_pos.y + 4));

  size_t pos = 0;
  while (pos < text.size()) {
    size_t nl = text.find('\n', pos);
    std::string line = (nl == std::string::npos) ? text.substr(pos) : text.substr(pos, nl - pos);
    ImGui::TextUnformatted(line.c_str());
    if (nl == std::string::npos)
      break;
    pos = nl + 1;
    ImGui::SetCursorScreenPos(ImVec2(start_pos.x + 8, ImGui::GetCursorScreenPos().y));
  }

  ImGui::PopStyleColor();

  ImGui::SetCursorScreenPos(ImVec2(start_pos.x, start_pos.y + height + 8));
}

static void start_chat(AsyncChatState& chat, ChatSession& session, std::string input) {
  chat.running = true;
  chat.future = std::async(std::launch::async, [&session, input = std::move(input)]() { return session.run_once(input); });
}

static void cancel_chat(AsyncChatState& chat) {
  g_interrupted = true;
  if (chat.future.valid()) {
    chat.future.wait();
    chat.future.get();
  }
  chat.running = false;
  g_interrupted = false;
}

void render_chat_ui(ChatUIState& ui, AsyncChatState& chat, ChatSession& session, bool& done) {
  // ── drain pending output from chat thread ──
  {
    std::lock_guard<std::mutex> lock(chat.mutex);
    for (auto& [text, type] : chat.pending) {
      if (type == OutputType::ToolInvocation) {
        if (!ui.entries.empty() && ui.entries.back().is_streaming) {
          ui.entries.back().is_streaming = false;
        }
        ui.entries.push_back({EntryType::ToolCall, text, false});
      } else {
        auto entry_type = (type == OutputType::Reasoning) ? EntryType::Reasoning : EntryType::Content;
        if (!ui.entries.empty() && ui.entries.back().is_streaming && ui.entries.back().type == entry_type) {
          ui.entries.back().text += text;
        } else {
          if (!ui.entries.empty() && ui.entries.back().is_streaming) {
            ui.entries.back().is_streaming = false;
          }
          ui.entries.push_back({entry_type, text, true});
        }
      }
    }
    chat.pending.clear();
  }

  // ── check if chat just finished ──
  if (chat.running && chat.future.valid() && chat.future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    auto result = chat.future.get();
    chat.running = false;
    if (!ui.entries.empty() && ui.entries.back().is_streaming) {
      ui.entries.back().is_streaming = false;
    }
    if (!result) {
      ui.entries.push_back({EntryType::Content, "Error: " + result.error(), false});
    }
  }

  // ── main window ──
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
  ImGui::Begin(
      "llm-chat", nullptr, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

  // ── menu bar ──
  if (ImGui::BeginMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("Exit", "Alt+F4"))
        done = true;
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
      if (ImGui::MenuItem("Clear conversation")) {
        session.clear();
        ui.entries.clear();
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Model")) {
      bool changed = ImGui::InputText("Name", ui.model_buf, sizeof(ui.model_buf), ImGuiInputTextFlags_EnterReturnsTrue);
      ImGui::SameLine();
      if (ImGui::Button("Apply") || changed) {
        session.set_model(ui.model_buf);
      }
      ImGui::Text("Current: %s", session.model().c_str());
      ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
  }

  // ── conversation area ──
  float input_height = ImGui::GetFrameHeightWithSpacing() * 3 + ImGui::GetStyle().ItemSpacing.y * 2 + ImGui::GetFrameHeightWithSpacing() + 8;

  if (ui.mono_font)
    ImGui::PushFont(ui.mono_font);
  ImGui::BeginChild("##msgs", ImVec2(0, -input_height), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

  for (size_t i = 0; i < ui.entries.size(); i++) {
    auto& entry = ui.entries[i];

    // deepseek returning empty blocks?
    if (entry.type == EntryType::Content && !entry.text.size()) continue;

    if (i > 0) ImGui::NewLine();
    ImGui::PushID(std::string("entry-" + std::to_string(i)).c_str());

    switch (entry.type) {
    case EntryType::UserText:
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(100, 180, 255, 255));
      ImGui::TextWrapped("You: %s", entry.text.c_str());
      ImGui::PopStyleColor();
      break;
    case EntryType::Reasoning:
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 160, 160, 255));
      render_content(ui.mono_font, ui.mono_font, "Thinking: " + entry.text);
      ImGui::PopStyleColor();
      break;
    case EntryType::Content:
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetColorU32(ImGuiCol_Text));
      render_content(ui.mono_font, ui.mono_font, entry.text);
      ImGui::PopStyleColor();
      break;
    case EntryType::ToolCall:
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 165, 0, 255));
      ImGui::TextWrapped("%s", entry.text.c_str());
      // collapse tool calls together
      for (; i+1 < ui.entries.size() && ui.entries[i+1].type == EntryType::ToolCall; i++) {
        ImGui::TextWrapped("%s", ui.entries[i+1].text.c_str());
      }
      ImGui::PopStyleColor();
      break;
    }

    ImGui::PopID();
  }

  // auto-scroll
  float scroll_y = ImGui::GetScrollY();
  float scroll_max = ImGui::GetScrollMaxY();
  if (scroll_y >= scroll_max - 10.0f) {
    ui.auto_scroll = true;
  } else {
    ui.auto_scroll = false;
  }

  if (ui.auto_scroll) {
    ImGui::SetScrollHereY(1.0f);
  }

  ImGui::EndChild(); // ##msgs
  if (ui.mono_font)
    ImGui::PopFont();

  // ── input area ──
  ImGui::Separator();

  bool running_snapshot = chat.running;

  if (running_snapshot) {
    float cancel_w = ImGui::CalcTextSize("  Cancel  ").x + ImGui::GetStyle().FramePadding.x * 2;
    float input_w = ImGui::GetContentRegionAvail().x - cancel_w - ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetNextItemWidth(input_w);
  } else {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
  }

  if (running_snapshot)
    ImGui::BeginDisabled();
  if (ui.mono_font)
    ImGui::PushFont(ui.mono_font);

  if (ImGui::InputTextMultiline("##input",
                                ui.input_buf,
                                sizeof(ui.input_buf),
                                ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 3),
                                ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_EnterReturnsTrue)) {
    std::string input(ui.input_buf);
    ui.input_buf[0] = '\0';
    if (!input.empty()) {
      if (input == "/clear") {
        session.clear();
        ui.entries.clear();
      } else if (input == "/exit" || input == "/quit") {
        done = true;
      } else {
        ui.entries.push_back({EntryType::UserText, input, false});
        start_chat(chat, session, std::move(input));
      }
    }
  }

  if (ui.mono_font)
    ImGui::PopFont();
  if (running_snapshot) {
    ImGui::EndDisabled();
  } else if (chat.running) {
    ImGui::SameLine();
  }

  if (chat.running) {
    if (ImGui::Button("  Cancel  ")) {
      cancel_chat(chat);
    }
  }

  ImGui::End(); // main window
}
