#include <assets_snapshot.h>
#include <inttypes.h>
#include <math.h>
#include <wally_elements.h>
#include <wally_transaction.h>

#include "../button_events.h"
#include "../jade_assert.h"
#include "../jade_wally_verify.h"
#include "../ui.h"
#include "../utils/address.h"
#include "../utils/event.h"
#include "../utils/network.h"

// A warning to display if the asset registry data is missing
static const char MISSING_ASSET_DATA[] = "Amounts may be expressed in the wrong units. Proceed at your own risk.";

// A warning to display if the unblinding data is missing
static const char BLINDED_OUTPUT[] = "Output cannot be unblinded!";

// Translate a GUI button (ok/cancel) into a sign_tx_ JADE_EVENT (so the caller
// can await without worrying about which screen/activity it came from).
static void translate_event(void* handler_arg, esp_event_base_t base, int32_t id, void* unused)
{
    JADE_ASSERT(id == BTN_TX_SCREEN_EXIT || id == BTN_TX_SCREEN_NEXT);
    esp_event_post(JADE_EVENT, id == BTN_TX_SCREEN_NEXT ? SIGN_TX_ACCEPT_OUTPUTS : SIGN_TX_DECLINE, NULL, 0,
        100 / portTICK_PERIOD_MS);
}

// Helper to make a screen activity to display an input or output for the user to verify.
// Displays a label or a destination address, passed amount (already formatted for display),
// and the associated ticker if one is passed.
//
// It can also display one of:
// a) Asset string (eg. issuer + asset-id) for liquid registered assets, or
// b) any warning message that may be associated with this output.
//
// Due to screen real-estate / visual overcrowding issues it was decided that liquid
// outputs that have both asset data *and* a warning message would be displayed twice
// (once with the warning, and again with the asset info) rather than trying to squeeze
// all the information onto the screen a once.
//
// So it is not valid to call this with both asset_str and warning_msg.
// Nor is it valid to call this with both an address and a label string.
//
static void make_input_output_activity(link_activity_t* output_activity, const char* title, const bool want_prev_btn,
    const char* address, const char* label, const char* amount, const char* ticker, const char* asset_str,
    const char* warning_msg)
{
    JADE_ASSERT(output_activity);
    JADE_ASSERT(title);
    JADE_ASSERT(!address || !label);
    JADE_ASSERT(amount);
    JADE_ASSERT(ticker);
    JADE_ASSERT(!asset_str || !warning_msg);

    gui_activity_t* act = NULL;
    gui_make_activity(&act, true, title);

    gui_view_node_t* vsplit = NULL;
    const bool have_additional_info = asset_str || warning_msg;
    if (!have_additional_info) {
        // Just showing amount and ticker - eg. simple BTC tx/output, no warnings or asset-info.
        // In this case wrap address or label over multiple lines as required.
        gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 3, 44, 24, 32);
        gui_set_margins(vsplit, GUI_MARGIN_TWO_VALUES, 8, 4);
        gui_set_parent(vsplit, act->root_node);

        if (address) {
            gui_view_node_t* hsplit_text1;
            gui_make_hsplit(&hsplit_text1, GUI_SPLIT_RELATIVE, 2, 12, 88);
            gui_set_parent(hsplit_text1, vsplit);

            gui_view_node_t* vsplit1a;
            gui_make_vsplit(&vsplit1a, GUI_SPLIT_RELATIVE, 2, 35, 65);
            gui_set_parent(vsplit1a, hsplit_text1);

            gui_view_node_t* text1a;
            gui_make_text(&text1a, "To", TFT_WHITE);
            gui_set_parent(text1a, vsplit1a);
            gui_set_align(text1a, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
            gui_set_margins(text1a, GUI_MARGIN_TWO_VALUES, 0, 4);
            gui_set_borders(text1a, TFT_BLOCKSTREAM_GREEN, 2, GUI_BORDER_BOTTOM);

            gui_view_node_t* text1b;
            gui_make_text(&text1b, address, TFT_WHITE);
            gui_set_parent(text1b, hsplit_text1);
            gui_set_padding(text1b, GUI_MARGIN_TWO_VALUES, 0, 4);
            gui_set_align(text1b, GUI_ALIGN_RIGHT, GUI_ALIGN_TOP);
        } else if (label) {
            gui_view_node_t* text1;
            gui_make_text(&text1, label, TFT_WHITE);
            gui_set_parent(text1, vsplit);
            gui_set_padding(text1, GUI_MARGIN_TWO_VALUES, 0, 4);
            gui_set_align(text1, GUI_ALIGN_CENTER, GUI_ALIGN_TOP);
        } else {
            // row1 is blank
            gui_view_node_t* row1;
            gui_make_fill(&row1, TFT_BLACK);
            gui_set_parent(row1, vsplit);
        }
    } else {
        // More data to show - liquid asset info or maybe a text warning
        // In that case the address or label is scrolling on a single line.
        gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 5, 17, 17, 17, 17, 32);
        gui_set_margins(vsplit, GUI_MARGIN_TWO_VALUES, 2, 2);
        gui_set_parent(vsplit, act->root_node);

        if (address) {
            gui_view_node_t* hsplit_text1;
            gui_make_hsplit(&hsplit_text1, GUI_SPLIT_RELATIVE, 2, 15, 85);
            gui_set_parent(hsplit_text1, vsplit);

            gui_view_node_t* text1a;
            gui_make_text(&text1a, "To", TFT_WHITE);
            gui_set_parent(text1a, hsplit_text1);
            gui_set_align(text1a, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
            gui_set_borders(text1a, TFT_BLOCKSTREAM_GREEN, 2, GUI_BORDER_BOTTOM);

            // Constrained to scrolling on one line
            char display_address[MAX_ADDRESS_LEN + 4];
            const int ret = snprintf(display_address, sizeof(display_address), "} %s {", address);
            JADE_ASSERT(ret > 0 && ret < sizeof(display_address));

            gui_view_node_t* text1b;
            gui_make_text(&text1b, display_address, TFT_WHITE);
            gui_set_parent(text1b, hsplit_text1);
            gui_set_align(text1b, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
            gui_set_text_scroll(text1b, TFT_BLACK);
        } else if (label) {
            gui_view_node_t* text1;
            gui_make_text(&text1, label, TFT_WHITE);
            gui_set_parent(text1, vsplit);
            gui_set_padding(text1, GUI_MARGIN_TWO_VALUES, 0, 4);
            gui_set_align(text1, GUI_ALIGN_CENTER, GUI_ALIGN_TOP);
        } else {
            // row1 is blank
            gui_view_node_t* row1;
            gui_make_fill(&row1, TFT_BLACK);
            gui_set_parent(row1, vsplit);
        }
    }

    {
        // row2 is amount and ticker
        gui_view_node_t* hsplit_text2;
        gui_make_hsplit(&hsplit_text2, GUI_SPLIT_RELATIVE, 2, 70, 30);
        gui_set_parent(hsplit_text2, vsplit);

        gui_view_node_t* text2a;
        gui_make_text(&text2a, amount, TFT_WHITE);
        gui_set_parent(text2a, hsplit_text2);
        gui_set_align(text2a, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);

        gui_view_node_t* text2b;
        gui_make_text(&text2b, ticker, TFT_WHITE);
        gui_set_parent(text2b, hsplit_text2);
        gui_set_align(text2b, GUI_ALIGN_RIGHT, GUI_ALIGN_MIDDLE);
        gui_set_borders(text2b, TFT_BLOCKSTREAM_GREEN, 2, GUI_BORDER_BOTTOM);
    }

    // If 'warning_msg' - then show the message.
    // Otherwise show the asset string (issuer, id, etc)
    if (warning_msg) {
        JADE_ASSERT(!asset_str);

        gui_view_node_t* text3;
        gui_make_text(&text3, "Warning:", TFT_RED);
        gui_set_parent(text3, vsplit);
        gui_set_align(text3, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
        gui_set_text_scroll(text3, TFT_BLACK);

        gui_view_node_t* text4;
        gui_make_text(&text4, warning_msg, TFT_RED);
        gui_set_parent(text4, vsplit);
        gui_set_align(text4, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
        gui_set_text_scroll(text4, TFT_BLACK);
    } else if (asset_str) {
        gui_view_node_t* hsplit_text3;
        gui_make_hsplit(&hsplit_text3, GUI_SPLIT_RELATIVE, 2, 30, 70);
        gui_set_parent(hsplit_text3, vsplit);

        gui_view_node_t* text3a;
        gui_make_text(&text3a, "Asset", TFT_WHITE);
        gui_set_parent(text3a, hsplit_text3);
        gui_set_align(text3a, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);

        gui_view_node_t* text3b;
        gui_make_text(&text3b, asset_str, TFT_WHITE);
        gui_set_parent(text3b, hsplit_text3);
        gui_set_align(text3b, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
        gui_set_text_scroll(text3b, TFT_BLACK);

        // row4 is blank
        gui_view_node_t* row4;
        gui_make_fill(&row4, TFT_BLACK);
        gui_set_parent(row4, vsplit);
    } else {
        JADE_ASSERT(!have_additional_info);
    }

    // Buttons
    btn_data_t btns[] = { { .txt = "=", .font = JADE_SYMBOLS_16x16_FONT, .ev_id = BTN_TX_SCREEN_PREV },
        { .txt = "X", .font = DEFAULT_FONT, .ev_id = BTN_TX_SCREEN_EXIT },
        { .txt = "S", .font = VARIOUS_SYMBOLS_FONT, .ev_id = BTN_TX_SCREEN_NEXT } };

    // Remove 'Previous' button if not valid
    if (!want_prev_btn) {
        btns[0].txt = NULL;
        btns[0].ev_id = GUI_BUTTON_EVENT_NONE;
    }

    add_buttons(vsplit, UI_ROW, btns, 3);

    // Connect every screen's 'exit' button to the 'translate' handler above
    gui_activity_register_event(act, GUI_BUTTON_EVENT, BTN_TX_SCREEN_EXIT, translate_event, NULL);

    // Set the intially selected item to the 'Next' button (ie. btns[2])
    gui_set_activity_initial_selection(act, btns[2].btn);

    // Push details into the output structure
    output_activity->activity = act;
    output_activity->prev_button = btns[0].btn;
    output_activity->next_button = btns[2].btn;
}

static void make_final_activity(gui_activity_t** activity_ptr, const char* title, const char* total_fee,
    const char* ticker, const char* warning_msg)
{
    JADE_ASSERT(activity_ptr);
    JADE_ASSERT(title);
    JADE_ASSERT(total_fee);
    JADE_ASSERT(ticker);

    gui_make_activity(activity_ptr, true, title);

    gui_view_node_t* vsplit;
    gui_make_vsplit(&vsplit, GUI_SPLIT_RELATIVE, 4, 22, 22, 22, 34);
    gui_set_padding(vsplit, GUI_MARGIN_ALL_DIFFERENT, 2, 2, 2, 2);
    gui_set_parent(vsplit, (*activity_ptr)->root_node);

    gui_view_node_t* hsplit1;
    gui_make_hsplit(&hsplit1, GUI_SPLIT_RELATIVE, 2, 20, 80);
    gui_set_parent(hsplit1, vsplit);

    gui_view_node_t* text1;
    gui_make_text(&text1, "Fee", TFT_WHITE);
    gui_set_parent(text1, hsplit1);
    gui_set_align(text1, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
    gui_set_borders(text1, TFT_BLOCKSTREAM_GREEN, 2, GUI_BORDER_BOTTOM);

    gui_view_node_t* text1b;
    char tx_fees[32];
    const int ret = snprintf(tx_fees, sizeof(tx_fees), "%s %s", total_fee, ticker);
    JADE_ASSERT(ret > 0 && ret < sizeof(tx_fees));
    gui_make_text(&text1b, tx_fees, TFT_WHITE);
    gui_set_parent(text1b, hsplit1);
    gui_set_align(text1b, GUI_ALIGN_RIGHT, GUI_ALIGN_MIDDLE);

    // Show any warning message
    if (warning_msg) {
        gui_view_node_t* text2;
        gui_make_text(&text2, "Warning:", TFT_RED);
        gui_set_parent(text2, vsplit);
        gui_set_align(text2, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
        gui_set_text_scroll(text2, TFT_BLACK);

        gui_view_node_t* text3;
        gui_make_text(&text3, warning_msg, TFT_RED);
        gui_set_parent(text3, vsplit);
        gui_set_align(text3, GUI_ALIGN_LEFT, GUI_ALIGN_MIDDLE);
        gui_set_text_scroll(text3, TFT_BLACK);
    } else {
        // Two blank rows
        gui_view_node_t* row2;
        gui_make_fill(&row2, TFT_BLACK);
        gui_set_parent(row2, vsplit);

        gui_view_node_t* row3;
        gui_make_fill(&row3, TFT_BLACK);
        gui_set_parent(row3, vsplit);
    }

    // Buttons
    btn_data_t btns[] = { { .txt = "X", .font = DEFAULT_FONT, .ev_id = BTN_CANCEL_SIGNATURE },
        { .txt = NULL, .font = DEFAULT_FONT, .ev_id = GUI_BUTTON_EVENT_NONE }, // spacer
        { .txt = "S", .font = VARIOUS_SYMBOLS_FONT, .ev_id = BTN_ACCEPT_SIGNATURE } };
    add_buttons(vsplit, UI_ROW, btns, 3);
}

// Don't display pre-validated (eg. change) outputs (if provided) unless they have an associated warning message.
// Should work for elements and standard btc, but liquid hides scriptless outputs (fees)
static bool display_output(
    const struct wally_tx_output* outputs, const output_info_t* output_info, const size_t i, const bool show_scriptless)
{
    if (!show_scriptless && !outputs[i].script) {
        // Hide outputs with no script
        return false;
    }

    if (output_info) {
        if (output_info[i].message[0] != '\0') {
            // Show outputs that have an associated warning message
            return true;
        }

        if (output_info[i].flags & OUTPUT_FLAG_VALIDATED && output_info[i].flags & OUTPUT_FLAG_CHANGE) {
            // Hide change outputs which have already been internally validated
            return false;
        }
    }

    // No reason to hide this output
    return true;
}

static uint32_t displayable_outputs(
    const struct wally_tx* tx, const output_info_t* output_info, const bool show_scriptless)
{
    uint32_t nDisplayable = 0;
    for (size_t i = 0; i < tx->num_outputs; ++i) {
        if (display_output(tx->outputs, output_info, i, show_scriptless)) {
            ++nDisplayable;
        }
    }

    // If we would hide all outputs, then don't hide any
    return nDisplayable > 0 ? nDisplayable : tx->num_outputs;
}

void make_display_output_activity(
    const char* network, const struct wally_tx* tx, const output_info_t* output_info, gui_activity_t** first_activity)
{
    JADE_ASSERT(network);
    JADE_ASSERT(tx);
    // Note: output_info is optional and can be null
    JADE_ASSERT(first_activity);

    // Show outputs which don't have a script
    const bool show_scriptless = true;

    // Chain the output activities
    link_activity_t output_act = {};
    linked_activities_info_t act_info = {};

    // 1 based indices for display purposes
    uint32_t nDisplayedOutput = 0;
    const uint32_t nTotalOutputsDisplayed = displayable_outputs(tx, output_info, show_scriptless);
    const bool hiddenOutputs = nTotalOutputsDisplayed < tx->num_outputs;

    for (size_t i = 0; i < tx->num_outputs; ++i) {
        struct wally_tx_output* out = tx->outputs + i;

        // Skip outputs we have automatically validated (eg. change outputs)
        if (hiddenOutputs && !display_output(tx->outputs, output_info, i, show_scriptless)) {
            continue;
        }
        ++nDisplayedOutput;

        char title[16];
        int ret = snprintf(title, sizeof(title), "Output %ld/%ld", nDisplayedOutput, nTotalOutputsDisplayed);
        JADE_ASSERT(ret > 0 && ret < sizeof(title));

        char amount[32];
        ret = snprintf(amount, sizeof(amount), "%.08f", 1.0 * out->satoshi / 1e8);
        JADE_ASSERT(ret > 0 && ret < sizeof(amount));

        char address[MAX_ADDRESS_LEN];
        script_to_address(network, out->script, out->script_len, address, sizeof(address));

        const char* msg = output_info && strlen(output_info[i].message) > 0 ? output_info[i].message : NULL;
        make_input_output_activity(&output_act, title, act_info.last_activity, address, NULL, amount, "BTC", NULL, msg);
        gui_chain_activities(&output_act, &act_info);
    }
    JADE_ASSERT(nDisplayedOutput == nTotalOutputsDisplayed);

    // Connect the final screen's 'next' button to the 'translate' handler above
    gui_activity_register_event(act_info.last_activity, GUI_BUTTON_EVENT, BTN_TX_SCREEN_NEXT, translate_event, NULL);

    *first_activity = act_info.first_activity;
}

static bool get_asset_display_info(const char* network, const asset_info_t* assets, const size_t num_assets,
    const uint8_t* asset_id, const size_t asset_id_len, const uint64_t value, char* asset_str,
    const size_t asset_str_len, char* amount, const size_t amount_len, char* ticker, const size_t ticker_len)
{
    JADE_ASSERT(network);
    JADE_ASSERT(assets || !num_assets);
    JADE_ASSERT(asset_id);
    JADE_ASSERT(asset_id_len);
    JADE_ASSERT(asset_str);
    JADE_ASSERT(amount);
    JADE_ASSERT(ticker);

    // Get the asset-id display hex string
    char* asset_id_hex = NULL;
    JADE_WALLY_VERIFY(wally_hex_from_bytes(asset_id, asset_id_len, &asset_id_hex));
    JADE_ASSERT(asset_id_hex);

    // Look up the asset-id in the canned asset-data
    asset_info_t asset_info = {};
    const bool have_asset_info = assets_get_info(network, assets, num_assets, asset_id_hex, &asset_info);
    if (have_asset_info) {
        JADE_LOGI("Found asset data for asset-id: '%s'", asset_id_hex);

        // Issuer and asset-id concatenated
        int ret = snprintf(asset_str, asset_str_len, "} %.*s - %s {", asset_info.issuer_domain_len,
            asset_info.issuer_domain, asset_id_hex);
        JADE_ASSERT(ret > 0);
        asset_str[asset_str_len - 1] = '\0'; // Truncate/terminate if necessary

        // Amount scaled and displayed at relevant precision
        const uint32_t scale_factor = pow(10, asset_info.precision);
        ret = snprintf(amount, amount_len, "%.*f", asset_info.precision, 1.0 * value / scale_factor);
        JADE_ASSERT(ret > 0 && ret < amount_len);

        // Ticker
        ret = snprintf(ticker, ticker_len, "%.*s", asset_info.ticker_len, asset_info.ticker);
        JADE_ASSERT(ret > 0 && ret < ticker_len);
    } else {
        JADE_LOGW("Asset data for asset-id: '%s' not found!", asset_id_hex);

        // Issuer unknown
        int ret = snprintf(asset_str, asset_str_len, "} issuer unknown - %s {", asset_id_hex);
        JADE_ASSERT(ret > 0 && ret < asset_str_len);

        // sats precision
        ret = snprintf(amount, amount_len, "%.00f", 1.0 * value);
        JADE_ASSERT(ret > 0 && ret < amount_len);

        // No ticker
        ticker[0] = '\0';
    }
    JADE_WALLY_VERIFY(wally_free_string(asset_id_hex));

    return have_asset_info;
}

void make_display_elements_output_activity(const char* network, const struct wally_tx* tx,
    const output_info_t* output_info, const asset_info_t* assets, const size_t num_assets,
    gui_activity_t** first_activity)
{
    JADE_ASSERT(network);
    JADE_ASSERT(tx);
    JADE_ASSERT(output_info);
    JADE_ASSERT(assets || !num_assets);
    JADE_ASSERT(first_activity);

    // Don't show outputs which don't have a script (as these are fees)
    const bool show_scriptless = false;

    // Track the first and last activities created
    link_activity_t output_act = {};
    linked_activities_info_t act_info = {};

    // 1 based indices for display purposes
    uint32_t nDisplayedOutput = 0;
    const uint32_t nTotalOutputsDisplayed = displayable_outputs(tx, output_info, show_scriptless);
    const bool hiddenOutputs = nTotalOutputsDisplayed < tx->num_outputs;

    for (size_t i = 0; i < tx->num_outputs; ++i) {
        struct wally_tx_output* out = tx->outputs + i;

        // Skip outputs we have automatically validated (eg. change outputs)
        // also, skip/hide fees (ie. outputs sans script)
        if (hiddenOutputs && !display_output(tx->outputs, output_info, i, show_scriptless)) {
            continue;
        }
        ++nDisplayedOutput;

        char title[16];
        const int ret = snprintf(title, sizeof(title), "Output %ld/%ld", nDisplayedOutput, nTotalOutputsDisplayed);
        JADE_ASSERT(ret > 0 && ret < sizeof(title));

        // Get the address
        char address[MAX_ADDRESS_LEN];
        elements_script_to_address(network, out->script, out->script_len,
            (output_info[i].flags & OUTPUT_FLAG_HAS_BLINDING_KEY) ? output_info[i].blinding_key : NULL,
            sizeof(output_info[i].blinding_key), address, sizeof(address));

        // If there is no unblinded info, make warning/placeholder screen
        // ATM assert that we always have unblinded info when displaying an output
        JADE_ASSERT(output_info[i].flags & OUTPUT_FLAG_HAS_UNBLINDED);
        if (!(output_info[i].flags & OUTPUT_FLAG_HAS_UNBLINDED)) {
            make_input_output_activity(&output_act, title, act_info.last_activity, address, NULL, "????", "????",
                "????????????", BLINDED_OUTPUT);
            gui_chain_activities(&output_act, &act_info);
            continue;
        }

        // Look up the asset-id in the canned asset-data
        char asset_str[128];
        char amount[32];
        char ticker[8]; // Registry tickers are max 5char ... but testnet policy asset ticker is 'L-TEST' ...
        const bool have_asset_info = get_asset_display_info(network, assets, num_assets, output_info[i].asset_id,
            sizeof(output_info[i].asset_id), output_info[i].value, asset_str, sizeof(asset_str), amount, sizeof(amount),
            ticker, sizeof(ticker));

        // Insert extra screen to display warning message for this output, if one is passed
        if (strlen(output_info[i].message) > 0) {
            // Make activity with no asset-id but with the warning message
            make_input_output_activity(&output_act, title, act_info.last_activity, address, NULL, amount, ticker, NULL,
                output_info[i].message);
            gui_chain_activities(&output_act, &act_info);
        }

        // Insert extra screen to display warning if the asset registry information is missing
        if (!have_asset_info) {
            // Make activity with no asset-id but with the warning message
            make_input_output_activity(
                &output_act, title, act_info.last_activity, address, NULL, amount, ticker, NULL, MISSING_ASSET_DATA);
            gui_chain_activities(&output_act, &act_info);
        }

        // Normal output screen - with issuer and asset-id but no warning message
        make_input_output_activity(
            &output_act, title, act_info.last_activity, address, NULL, amount, ticker, asset_str, NULL);
        gui_chain_activities(&output_act, &act_info);
    }
    JADE_ASSERT(nDisplayedOutput == nTotalOutputsDisplayed);

    // Connect the final screen's 'next' button to the 'translate' handler above
    gui_activity_register_event(act_info.last_activity, GUI_BUTTON_EVENT, BTN_TX_SCREEN_NEXT, translate_event, NULL);

    // Set output parameters
    *first_activity = act_info.first_activity;
}

static void make_elements_asset_summary_screens(linked_activities_info_t* act_info, const char* title,
    const char* direction, const char* network, const asset_info_t* assets, const size_t num_assets,
    const movement_summary_info_t* summary, const size_t summary_len)
{
    JADE_ASSERT(act_info);
    JADE_ASSERT(title);
    JADE_ASSERT(direction);
    JADE_ASSERT(network);
    JADE_ASSERT(assets || !num_assets);
    JADE_ASSERT(summary);
    JADE_ASSERT(summary_len);

    link_activity_t output_act;
    for (size_t i = 0; i < summary_len; ++i) {
        char label[16];
        if (summary_len == 1) {
            // Omit counter if just one intput/output
            const int ret = snprintf(label, sizeof(label), "%s", direction);
            JADE_ASSERT(ret > 0 && ret < sizeof(label));
        } else {
            // 1 based indices for display purposes
            const int ret = snprintf(label, sizeof(label), "%s  (%d/%d)", direction, i + 1, summary_len);
            JADE_ASSERT(ret > 0 && ret < sizeof(label));
        }

        // Look up the asset-id in the canned asset-data
        char asset_str[128];
        char amount[32];
        char ticker[8]; // Registry tickers are max 5char ... but testnet policy asset ticker is 'L-TEST' ...
        const bool have_asset_info
            = get_asset_display_info(network, assets, num_assets, summary[i].asset_id, sizeof(summary[i].asset_id),
                summary[i].value, asset_str, sizeof(asset_str), amount, sizeof(amount), ticker, sizeof(ticker));

        // Insert extra screen to display warning if the asset registry information is missing
        if (!have_asset_info) {
            // Make activity with no asset-id but with the warning message
            make_input_output_activity(
                &output_act, title, act_info->last_activity, NULL, label, amount, ticker, NULL, MISSING_ASSET_DATA);
            gui_chain_activities(&output_act, act_info);
        }

        // Normal output screen - with issuer and asset-id but no warning message
        make_input_output_activity(
            &output_act, title, act_info->last_activity, NULL, label, amount, ticker, asset_str, NULL);
        gui_chain_activities(&output_act, act_info);
    }
}

void make_display_elements_swap_activity(const char* network, const bool initial_proposal,
    const movement_summary_info_t* wallet_input_summary, const size_t wallet_input_summary_size,
    const movement_summary_info_t* wallet_output_summary, const size_t wallet_output_summary_size,
    const asset_info_t* assets, const size_t num_assets, gui_activity_t** first_activity)
{
    JADE_ASSERT(network);
    JADE_ASSERT(wallet_input_summary);
    JADE_ASSERT(wallet_input_summary_size);
    JADE_ASSERT(wallet_output_summary);
    JADE_ASSERT(wallet_output_summary_size);
    JADE_ASSERT(assets || !num_assets);
    JADE_ASSERT(first_activity);

    // Track the first and last activities created
    linked_activities_info_t act_info
        = { .first_activity = NULL, .last_activity = NULL, .last_activity_next_button = NULL };

    const char* title = initial_proposal ? "Swap Proposal" : "Complete Swap";

    // Screens for what we are receiving from the swap (ie. our outputs, summarised)
    make_elements_asset_summary_screens(
        &act_info, title, "Receive", network, assets, num_assets, wallet_output_summary, wallet_output_summary_size);

    // Screens for what we are sending into the swap (ie. our inputs, summarised)
    make_elements_asset_summary_screens(
        &act_info, title, "Send", network, assets, num_assets, wallet_input_summary, wallet_input_summary_size);

    // Connect the final screen's 'next' button to the 'translate' handler above
    gui_activity_register_event(act_info.last_activity, GUI_BUTTON_EVENT, BTN_TX_SCREEN_NEXT, translate_event, NULL);

    // Set output parameters
    *first_activity = act_info.first_activity;
}

// Screens to confirm the fee / signing the tx
void make_display_final_confirmation_activity(const uint64_t fee, const char* warning_msg, gui_activity_t** activity)
{
    JADE_ASSERT(activity);

    char fee_str[32];
    const int ret = snprintf(fee_str, sizeof(fee_str), "%.08f", 1.0 * fee / 1e8);
    JADE_ASSERT(ret > 0 && ret < sizeof(fee_str));

    // final confirmation screen
    make_final_activity(activity, "Summary", fee_str, "BTC", warning_msg);
}

void make_display_elements_final_confirmation_activity(
    const char* network, const char* title, const uint64_t fee, const char* warning_msg, gui_activity_t** activity)
{
    JADE_ASSERT(network);
    JADE_ASSERT(title);
    JADE_ASSERT(activity);

    // final confirmation screen

    // Policy asset must be present in h/coded asset data, and it must have a 'ticker'
    const char* asset_id_hex = networkGetPolicyAsset(network);
    JADE_ASSERT(asset_id_hex);
    asset_info_t asset_info = {};
    const bool have_asset_info = assets_get_info(network, NULL, 0, asset_id_hex, &asset_info);
    JADE_ASSERT(have_asset_info);
    JADE_ASSERT(asset_info.ticker);
    JADE_ASSERT(asset_info.ticker_len);

    // Ticker
    char ticker[8]; // Registry tickers are max 5char ... but testnet policy asset ticker is 'L-TEST' ...
    int ret = snprintf(ticker, sizeof(ticker), "%.*s", asset_info.ticker_len, asset_info.ticker);
    JADE_ASSERT(ret > 0 && ret < sizeof(ticker));

    // Fee amount scaled and displayed at relevant precision
    char fee_str[32];
    const uint32_t scale_factor = pow(10, asset_info.precision);
    ret = snprintf(fee_str, sizeof(fee_str), "%.*f", asset_info.precision, 1.0 * fee / scale_factor);
    JADE_ASSERT(ret > 0 && ret < sizeof(fee_str));

    // final confirmation screen
    make_final_activity(activity, title, fee_str, ticker, warning_msg);
}
