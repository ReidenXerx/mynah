// The Linux model tiers (M5) and the benchmark that picks one. The tier
// logic is pure arithmetic — every case here pins a row of the decision
// table, so retuning a budget (Phase 0's measurement pass was skipped)
// changes a visible pin instead of silently reshaping which model a
// machine gets.

#include "vendor/doctest.h"

#include <optional>
#include <string>

#include "models/table.hpp"
#include "models/tiers.hpp"

using namespace mynah::models;

TEST_CASE("the model table maps tiers to models") {
    CHECK(model_for_tier("gpu") == &kTurbo);
    CHECK(model_for_tier("small") == &kSmall);
    CHECK(model_for_tier("base") == &kBase);
    CHECK(model_for_tier("nonsense") == nullptr);
}

TEST_CASE("find resolves aliases and filenames") {
    CHECK(find("small") == &kSmall);
    CHECK(find("ggml-small.bin") == &kSmall);
    CHECK(find("turbo") == nullptr); // "turbo" is a SHORT alias, not a table alias
    CHECK(find("large-v3-turbo") == &kTurbo);
    CHECK(find("ggml-large-v3-turbo.bin") == &kTurbo);
    CHECK(find("base") == &kBase);
    CHECK(find("vad") == nullptr);
    CHECK(find("ggml-silero-v5.1.2.bin") == &kSileroVad);
    CHECK(find("nonexistent-model") == nullptr);
}

TEST_CASE("every model in the table is honest about its source") {
    // P8: the core publishes URL and size; the front end downloads and
    // verifies. A table entry without a URL would strand the CLI.
    for (const ModelInfo* info : {&kTurbo, &kSmall, &kBase, &kSileroVad}) {
        CHECK(!info->url.empty());
        CHECK(info->url.rfind("https://", 0) == 0);
        CHECK(info->approximate_bytes > 0);
        // The filename is what lands on disk and what resolve() finds in
        // the search directories.
        CHECK(info->filename.find("ggml-") == 0);
        CHECK(info->filename.size() > 4);
        CHECK(std::string_view(info->filename).substr(info->filename.size() - 4) ==
              ".bin");
    }
}

TEST_CASE("choose_tier picks by budget, never by guesswork") {
    // A GPU that meets the budget wins.
    CHECK(choose_tier(true, std::optional<double>(1.5), std::optional<double>(4.0)) ==
          "gpu");
    // Turbo too slow for the gpu budget, small fast enough: the CPU tier.
    CHECK(choose_tier(true, std::optional<double>(2.5), std::optional<double>(1.0)) ==
          "small");
    // No GPU at all: small when it fits.
    CHECK(choose_tier(false, std::optional<double>(1.0), std::optional<double>(4.9)) ==
          "small");
    // Small too slow: base is the floor — a machine too slow for anything
    // still gets a working mynah.
    CHECK(choose_tier(false, std::optional<double>(9.0), std::optional<double>(9.0)) ==
          "base");
    CHECK(choose_tier(false, std::nullopt, std::nullopt) == "base");
    // A candidate that failed to load or run never promotes the tier above
    // it: turbo missing means small can still win on its own budget.
    CHECK(choose_tier(true, std::nullopt, std::optional<double>(4.0)) == "small");
    // The budgets themselves are pinned: they are starting points from the
    // migration plan until Phase 0's measurements replace them.
    CHECK(kGpuBudgetSeconds == 2.0);
    CHECK(kSmallBudgetSeconds == 5.0);
}

TEST_CASE("the budgets compare seconds per utterance, the unit the plan names") {
    // Boundary semantics: "gpu if <= 2 s" — the budget is inclusive.
    CHECK(choose_tier(true, std::optional<double>(kGpuBudgetSeconds),
                      std::optional<double>(kSmallBudgetSeconds)) == "gpu");
    CHECK(choose_tier(false, std::optional<double>(kSmallBudgetSeconds),
                      std::optional<double>(kSmallBudgetSeconds)) == "small");
    // One epsilon over: falls through.
    CHECK(choose_tier(false, std::nullopt, std::optional<double>(kSmallBudgetSeconds + 1e-9)) ==
          "base");
}