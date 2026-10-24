#include "tests/includes/settings_window_harness.h"
#include "tests/includes/test_framework.h"

#include "settings/native/native_page_voice_fields.h"

#include <set>
#include <typeinfo>

namespace
{
using namespace native_settings;
using namespace native_settings::voice_fields;
constexpr wchar_t kError[] = L"请先填写有效的 API Key。";

class FeedbackHost : public HostServices
{
  public:
    explicit FeedbackHost(msimeui::Window &window) : window_(window)
    {
    }
    HWND Hwnd() const override
    {
        return nullptr;
    }
    msimeui::Window *Window() const override
    {
        return &window_;
    }
    void ShowToast(const std::wstring &, bool) override
    {
    }
    void Confirm(const std::wstring &, std::function<void(bool)>, const std::wstring &, const std::wstring &,
                 const std::wstring &) override
    {
    }
    void ShowModal(VisualPtr, std::function<void()>) override
    {
    }
    void CloseModal() override
    {
    }
    void NavigateTo(const std::string &) override
    {
    }
    void ScrollIntoView(const msimeui::Visual &) override
    {
    }

  private:
    msimeui::Window &window_;
};

class FeedbackScene
{
  public:
    // AI 页会被 KeepAlive 保留，连同它引用的上下文一起保留到进程退出。
    static FeedbackScene &Open(float dpi = 96.0f)
    {
        return *new FeedbackScene(dpi);
    }

    template <class T> std::set<T *> Visible()
    {
        std::set<T *> found;
        for (float y = 0; y < 2400; y += 8)
            for (float x = 0; x < 800; x += 8)
                if (auto *visual = dynamic_cast<T *>(harness.VisualAt(x, y)))
                    found.insert(visual);
        return found;
    }

    size_t ErrorCount()
    {
        size_t count = 0;
        for (auto *label : Visible<Label>())
            if (label->GetText() == kError)
                ++count;
        return count;
    }

    size_t FeedbackCount()
    {
        std::set<StatusText *> statuses;
        for (auto *visual : Visible<msimeui::Visual>())
            for (auto *parent = visual; parent; parent = parent->GetParentVisual())
                if (auto *status = dynamic_cast<StatusText *>(parent))
                    statuses.insert(status);
        return statuses.size();
    }

    msimeui::RectF LabelBounds(const std::wstring &text)
    {
        for (auto *label : Visible<Label>())
            if (label->GetText() == text)
                return label->GetBounds();
        throw std::runtime_error("Expected visible feedback or field label");
    }

    std::string Start(bool connection, bool arrow = false)
    {
        if (arrow)
        {
            msimeui::Visual *box = nullptr;
            for (auto *visual : Visible<msimeui::Visual>())
                if (std::string(typeid(*visual).name()).find("ModelCombo") != std::string::npos)
                    box = visual;
            REQUIRE(box != nullptr);
            const auto bounds = box->GetBounds();
            harness.Click(bounds.x + bounds.width - 16, bounds.y + bounds.height * 0.5f);
        }
        else
        {
            PlainButton *button = connection ? testButton : fetchButton;
            REQUIRE(!button->IsDisabled());
            const auto bounds = button->GetBounds();
            harness.Click(bounds.x + bounds.width * 0.5f, bounds.y + bounds.height * 0.5f);
        }
        const std::string type = connection ? "apiCredentialTest" : "apiModelList";
        REQUIRE(!sent.empty());
        REQUIRE(sent.back().value("type", std::string()) == type);
        return sent.back()["data"].value("requestId", std::string());
    }

    void Reply(bool connection, const std::string &id, bool ok = false)
    {
        model.Receive(Json{{"type", connection ? "apiCredentialTestResult" : "apiModelListResult"},
                           {"requestId", id},
                           {"ok", ok},
                           {"message", ok ? "连接成功" : Narrow(kError)},
                           {"models", Json::array({"test-model"})}}
                          .dump());
        // 模型菜单可能盖住反馈区域，断言前关闭它，避免把遮挡误当成状态清理。
        if (ok && !connection)
            harness.Window().GetScene()->ClearPopups(true);
        harness.Layout();
    }

    void Snapshot(const Json &data)
    {
        model.Receive(Json{{"type", "configSnapshot"}, {"data", data}}.dump());
        harness.Layout();
    }

    SettingsWindowHarness harness;
    std::vector<Json> sent;
    SettingsModel model{[this](const std::string &raw) { sent.push_back(Json::parse(raw)); }};
    FeedbackHost host{harness.Window()};
    PageContext context{model, host};
    PlainButton *testButton = nullptr;
    PlainButton *fetchButton = nullptr;

  private:
    explicit FeedbackScene(float dpi) : harness(dpi, {800.0f, 2400.0f})
    {
        harness.SetRoot(BuildAiPage(context));
        Snapshot(Json{{"ai_assistant", {{"provider", "deepseek"}}}});
        for (auto *button : Visible<PlainButton>())
        {
            if (button->Text() == L"测试连接")
                testButton = button;
            if (button->Text() == L"查询模型")
                fetchButton = button;
        }
        REQUIRE(testButton != nullptr);
        REQUIRE(fetchButton != nullptr);
    }
};
} // namespace

TEST_CASE(settings_ai_latest_operation_replaces_feedback_in_both_orders)
{
    for (bool connection : {false, true})
    {
        auto &scene = FeedbackScene::Open();
        scene.Reply(connection, scene.Start(connection));
        REQUIRE_EQ(scene.ErrorCount(), size_t{1});
        const auto next = scene.Start(!connection);
        REQUIRE_EQ(scene.ErrorCount(), size_t{0});
        scene.Reply(!connection, next);
        REQUIRE_EQ(scene.ErrorCount(), size_t{1});
    }
}

TEST_CASE(settings_ai_feedback_stays_in_one_reserved_row_at_each_dpi)
{
    for (float dpi : {96.0f, 120.0f, 144.0f, 192.0f})
    {
        auto &scene = FeedbackScene::Open(dpi);
        const float nextRow = scene.LabelBounds(L"候选数量").y;
        const auto test = scene.Start(true);
        REQUIRE(std::fabs(scene.LabelBounds(L"候选数量").y - nextRow) < 0.1f);
        scene.Reply(true, test);
        const auto testFeedback = scene.LabelBounds(kError);
        const auto actions = scene.testButton->GetBounds();
        REQUIRE(testFeedback.y >= actions.y + actions.height);
        REQUIRE(std::fabs(scene.LabelBounds(L"候选数量").y - nextRow) < 0.1f);
        scene.Reply(false, scene.Start(false));
        const auto fetchFeedback = scene.LabelBounds(kError);
        REQUIRE(std::fabs(testFeedback.x - fetchFeedback.x) < 0.1f);
        REQUIRE(std::fabs(testFeedback.y - fetchFeedback.y) < 0.1f);
        REQUIRE(std::fabs(scene.LabelBounds(L"候选数量").y - nextRow) < 0.1f);
        scene.Snapshot(Json{{"ai_assistant", {{"provider", "siliconflow"}}}});
        REQUIRE_EQ(scene.FeedbackCount(), size_t{0});
        REQUIRE(std::fabs(scene.LabelBounds(L"候选数量").y - nextRow) < 0.1f);
    }
}

TEST_CASE(settings_shared_credential_test_keeps_default_config_change_feedback)
{
    // 使用未传 AI 页协调回调的构造方式，覆盖语音等页面沿用的默认行为。
    auto &scene = FeedbackScene::Open();
    auto button = std::make_shared<PlainButton>(L"测试连接", CredentialButtonStyle());
    auto status = std::make_shared<StatusText>();
    Json config = {{"token", ""}};
    CredentialTest test(scene.context, button, status, []() { return "voice.test"; }, [&]() { return config; });
    auto root = std::make_shared<Row>(10.0f);
    root->Add(button);
    root->Add(status);
    scene.harness.SetRoot(root);
    const auto bounds = button->GetBounds();
    scene.harness.Click(bounds.x + bounds.width * 0.5f, bounds.y + bounds.height * 0.5f);
    REQUIRE(button->IsDisabled());
    const auto id = scene.sent.back()["data"].value("requestId", std::string());
    config["token"] = "FAKESECRET_changed";
    scene.Reply(true, id);
    REQUIRE(!button->IsDisabled());
    bool changed = false;
    for (auto *label : scene.Visible<Label>())
        changed = changed || label->GetText() == L"配置已变更，请重新测试";
    REQUIRE(changed);
}

TEST_CASE(settings_ai_model_arrow_replaces_connection_feedback)
{
    auto &scene = FeedbackScene::Open();
    scene.Reply(true, scene.Start(true));
    scene.Reply(false, scene.Start(false, true));
    REQUIRE_EQ(scene.ErrorCount(), size_t{1});
}

TEST_CASE(settings_ai_superseded_responses_are_ignored_in_both_orders)
{
    for (bool connection : {false, true})
        for (bool oldFirst : {false, true})
            for (bool currentOk : {false, true})
            {
                auto &scene = FeedbackScene::Open();
                const auto old = scene.Start(connection);
                const auto current = scene.Start(!connection);
                REQUIRE(!(connection ? scene.testButton : scene.fetchButton)->IsDisabled());
                if (oldFirst)
                    scene.Reply(connection, old, !currentOk);
                scene.Reply(!connection, current, currentOk);
                if (!oldFirst)
                    scene.Reply(connection, old, !currentOk);
                REQUIRE_EQ(scene.ErrorCount(), currentOk ? size_t{0} : size_t{1});
                REQUIRE_EQ(scene.FeedbackCount(), size_t{1});
                REQUIRE(!scene.testButton->IsDisabled());
                REQUIRE(!scene.fetchButton->IsDisabled());
            }
}

TEST_CASE(settings_ai_configuration_changes_clear_feedback_and_invalidate_requests)
{
    const Json initial = {{"ai_assistant", {{"provider", "deepseek"}, {"tokens", {{"deepseek", ""}}}}}};
    for (const std::string field : {"provider", "token", "endpoint", "model"})
        for (bool connection : {false, true})
        {
            auto &scene = FeedbackScene::Open();
            scene.Snapshot(initial);
            scene.Reply(connection, scene.Start(connection));
            Json changed = initial;
            if (field == "provider")
                changed["ai_assistant"][field] = "siliconflow";
            else
                changed["ai_assistant"][field + "s"]["deepseek"] = "changed";
            scene.Snapshot(changed);
            REQUIRE_EQ(scene.ErrorCount(), size_t{0});
            scene.Snapshot(initial);
            const auto old = scene.Start(connection);
            scene.Snapshot(changed);
            REQUIRE(!scene.testButton->IsDisabled());
            REQUIRE(!scene.fetchButton->IsDisabled());
            // 切回原配置也不能让离开前的请求重新有效。
            scene.Snapshot(initial);
            scene.Reply(connection, old);
            REQUIRE_EQ(scene.ErrorCount(), size_t{0});
            REQUIRE_EQ(scene.FeedbackCount(), size_t{0});
        }
}

TEST_CASE(settings_ai_switching_mode_invalidates_both_operations)
{
    for (bool connection : {false, true})
    {
        auto &scene = FeedbackScene::Open();
        const auto old = scene.Start(connection);
        RadioOption *select = nullptr;
        for (auto *option : scene.Visible<RadioOption>())
            if (!option->IsChecked())
                select = option;
        REQUIRE(select != nullptr);
        const auto bounds = select->GetBounds();
        scene.harness.Click(bounds.x + 10, bounds.y + bounds.height * 0.5f);
        scene.Reply(connection, old);
        REQUIRE_EQ(scene.ErrorCount(), size_t{0});
        REQUIRE_EQ(scene.FeedbackCount(), size_t{0});
        REQUIRE(!scene.testButton->IsDisabled());
        REQUIRE(scene.fetchButton->IsDisabled());
    }
}

TEST_CASE(settings_ai_switching_provider_invalidates_both_operations)
{
    for (bool connection : {false, true})
    {
        auto &scene = FeedbackScene::Open();
        const auto old = scene.Start(connection);
        Dropdown *provider = nullptr;
        for (auto *dropdown : scene.Visible<Dropdown>())
            if (dropdown->GetValue() == "deepseek")
                provider = dropdown;
        REQUIRE(provider != nullptr);
        scene.harness.GiveWindowKeyboardFocus();
        const auto bounds = provider->GetBounds();
        scene.harness.Click(bounds.x + 10, bounds.y + bounds.height * 0.5f);
        scene.harness.Key(VK_DOWN);
        scene.harness.Key(VK_RETURN);
        REQUIRE(provider->GetValue() != "deepseek");
        scene.Reply(connection, old);
        REQUIRE_EQ(scene.ErrorCount(), size_t{0});
        REQUIRE_EQ(scene.FeedbackCount(), size_t{0});
        REQUIRE(!scene.testButton->IsDisabled());
        REQUIRE(!scene.fetchButton->IsDisabled());
    }
}

TEST_CASE(settings_ai_committing_request_fields_invalidates_feedback)
{
    for (const std::wstring field : {L"API Token", L"接口地址", L"模型"})
        for (bool connection : {false, true})
        {
            auto &scene = FeedbackScene::Open();
            scene.Reply(connection, scene.Start(connection));
            Label *label = nullptr;
            for (auto *candidate : scene.Visible<Label>())
                if (candidate->GetText() == field)
                    label = candidate;
            REQUIRE(label != nullptr);
            const auto labelBounds = label->GetBounds();
            msimeui::TextBox *input = nullptr;
            for (auto *candidate : scene.Visible<msimeui::TextBox>())
            {
                const auto bounds = candidate->GetBounds();
                if (bounds.x >= labelBounds.x && bounds.x < labelBounds.x + 40 &&
                    bounds.y >= labelBounds.y + labelBounds.height && bounds.y < labelBounds.y + 80)
                    input = candidate;
            }
            REQUIRE(input != nullptr);
            scene.harness.GiveWindowKeyboardFocus();
            const auto bounds = input->GetBounds();
            scene.harness.Click(bounds.x + 8, bounds.y + bounds.height * 0.5f);
            scene.harness.Type(L'x');
            scene.harness.Key(VK_RETURN);
            REQUIRE_EQ(scene.ErrorCount(), size_t{0});
            const auto old = scene.Start(connection);
            scene.harness.Click(bounds.x + 8, bounds.y + bounds.height * 0.5f);
            scene.harness.Type(L'y');
            scene.harness.Key(VK_RETURN);
            scene.Reply(connection, old);
            REQUIRE_EQ(scene.ErrorCount(), size_t{0});
            REQUIRE_EQ(scene.FeedbackCount(), size_t{0});
            REQUIRE(!scene.testButton->IsDisabled());
            REQUIRE(!scene.fetchButton->IsDisabled());
        }
}

TEST_CASE(settings_ai_unrelated_snapshot_keeps_current_feedback)
{
    auto &scene = FeedbackScene::Open();
    const Json initial = {{"ai_assistant", {{"provider", "deepseek"}}}};
    scene.Snapshot(initial);
    scene.Reply(true, scene.Start(true));
    Json changed = initial;
    changed["ai_assistant"]["candidate_limit"] = 5;
    scene.Snapshot(changed);
    REQUIRE_EQ(scene.ErrorCount(), size_t{1});
}
