#include "bf6_core.h"

#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: ui_options_provider_test <game-dir>\n");
        return 2;
    }
    char error[512]{};
    bf6_ctx* context = bf6_open(argv[1], error, (int)sizeof(error));
    if (!context)
    {
        std::fprintf(stderr, "open: %s\n", error);
        return 1;
    }
    if (!bf6_mount_frontend(context, error, (int)sizeof(error)))
    {
        std::fprintf(stderr, "mount: %s\n", error);
        bf6_close(context);
        return 1;
    }

    constexpr const char* display =
        "common/ui/options/ui/assets/categories/uioptionscategorydisplay";
    constexpr const char* video =
        "common/ui/options/ui/assets/categories/uioptionscategorydisplayvideo";
    int failures = 0;
    const int categoryCount = bf6_ui_option_subcategories(
        context, display, nullptr, 0);
    std::vector<bf6_ui_option_category> categories(
        categoryCount > 0 ? (size_t)categoryCount : 0);
    const int categoryFill = categoryCount > 0
        ? bf6_ui_option_subcategories(context, display, categories.data(),
                                      categoryCount)
        : categoryCount;
    std::printf("display subcategories=%d fill=%d\n", categoryCount,
                categoryFill);
    if (categoryCount != 13 || categoryFill != categoryCount) ++failures;
    if (categories.empty() ||
        std::strcmp(categories[0].id, "FID_DISPLAY_VIDEO") != 0 ||
        std::strcmp(categories[0].label, "Graphics") != 0 ||
        std::strcmp(categories[0].partition, video) != 0)
        ++failures;

    const int rowCount = bf6_ui_option_rows(context, video, nullptr, 0);
    std::vector<bf6_ui_option_row> rows(
        rowCount > 0 ? (size_t)rowCount : 0);
    const int rowFill = rowCount > 0
        ? bf6_ui_option_rows(context, video, rows.data(), rowCount)
        : rowCount;
    std::printf("graphics rows=%d fill=%d\n", rowCount, rowFill);
    if (rowCount != 27 || rowFill != rowCount) ++failures;
    if (rows.size() < 4) ++failures;
    else
    {
        std::printf("first=%s | %s=%s | %s=%s\n",
                    rows[0].label, rows[1].label, rows[1].value_label,
                    rows[2].label, rows[2].value_label);
        if (rows[0].kind != BF6_UI_OPTION_GROUP_TITLE ||
            std::strcmp(rows[0].label, "Graphics") != 0)
            ++failures;
        if (rows[1].kind != BF6_UI_OPTION_ENUM ||
            std::strcmp(rows[1].id, "PerformanceMode") != 0 ||
            !rows[1].label[0] || !rows[1].value_label[0] ||
            rows[1].enum_count != 3 || rows[1].authored_default_index != 0)
            ++failures;
        if (rows[2].kind != BF6_UI_OPTION_ENUM ||
            std::strcmp(rows[2].id, "OverallGraphicsQuality") != 0 ||
            rows[2].enum_count != 7)
            ++failures;
        if (rows[3].kind != BF6_UI_OPTION_SUBCATEGORY ||
            std::strcmp(rows[3].id, "FID_DISPLAY_GRAPHICS_SETTINGS") != 0)
            ++failures;
    }
    const int fakeCategories = bf6_ui_option_subcategories(
        context, "common/ui/options/__control_missing", nullptr, 0);
    const int fakeRows = bf6_ui_option_rows(
        context, "common/ui/options/__control_missing", nullptr, 0);
    std::printf("controls categories=%d rows=%d\n", fakeCategories, fakeRows);
    if (fakeCategories != -1 || fakeRows != -1) ++failures;

    bf6_close(context);
    std::printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
