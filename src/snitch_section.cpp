#include "snitch/snitch_section.hpp"

#include "snitch/snitch_console.hpp"
#include "snitch/snitch_registry.hpp"
#include "snitch/snitch_test_data.hpp"

#if SNITCH_WITH_EXCEPTIONS
#    include <exception>
#endif
#if SNITCH_WITH_TIMINGS
#    include <chrono>
#endif

namespace snitch::impl {
#if SNITCH_WITH_TIMINGS
using fsec         = std::chrono::duration<float>;
using snitch_clock = std::chrono::steady_clock;
#endif

section_entry_checker::~section_entry_checker() {
    auto& sections = state.info.sections;

    if (entered) {
#if SNITCH_WITH_EXCEPTIONS
        if (std::uncaught_exceptions() > 0 && !state.held_info.has_value()) {
            // We are unwinding the stack because an exception has been thrown;
            // keep a copy of the full section state since we will want to preserve the information
            // when reporting the exception.
            state.held_info = state.info;
        }
#endif

        pop_location(state);

        asserts          = state.asserts - asserts;
        failures         = state.failures - failures;
        allowed_failures = state.allowed_failures - allowed_failures;

        if (sections.depth == sections.levels.size()) {
            // We just entered this section, and there was no child section in it.
            // This is a leaf; flag that a leaf has been executed so that no other leaf
            // is executed in this run.
            // Note: don't pop this level from the section state yet, it may have siblings
            // that we don't know about yet. Popping will be done when we exit from the parent,
            // since then we will know if there is any sibling.
            sections.leaf_executed = true;
#if SNITCH_WITH_TIMINGS
            const auto end_time = snitch_clock::now().time_since_epoch();
            const auto duration =
                std::chrono::duration_cast<fsec>(end_time - snitch_clock::duration{start_time});
            state.reg.report_callback(
                state.reg, event::section_ended{
                               data.id, data.location, false, asserts, failures, allowed_failures,
                               duration.count()});
#else
            state.reg.report_callback(
                state.reg,
                event::section_ended{data.id, data.location, asserts, failures, allowed_failures});
#endif
        } else {
            // Check if there is any child section left to execute, at any depth below this one.
            bool no_child_section_left = true;
            for (std::size_t c = sections.depth; c < sections.levels.size(); ++c) {
                auto& child = sections.levels[c];
                if (child.previous_section_id != child.max_section_id) {
                    no_child_section_left = false;
                    break;
                }
            }

            if (no_child_section_left) {
                // No more children, we can pop this level and never go back.
                state.reg.report_callback(
                    state.reg, event::section_ended{
                                   sections.current_section.back().id,
                                   sections.current_section.back().location});
                sections.levels.pop_back();
            }
        }

        sections.current_section.pop_back();
    }

    --sections.depth;
}

section_entry_checker::operator bool() {
#if SNITCH_WITH_EXCEPTIONS
    state.held_info.reset();
#endif

    auto& sections = state.info.sections;

    if (sections.depth >= sections.levels.size()) {
        if (sections.depth >= max_nested_sections) {
            using namespace snitch::impl;
            state.reg.print(
                make_colored("error:", state.reg.with_color, color::fail),
                " max number of nested sections reached; "
                "please increase 'SNITCH_MAX_NESTED_SECTIONS' (currently ",
                max_nested_sections, ")\n.");
            assertion_failed("max number of nested sections reached");
        }

        sections.levels.push_back({});
    }

#if SNITCH_WITH_TIMINGS
    start_time = snitch_clock::now().time_since_epoch().count();
#endif
    ++sections.depth;
    asserts          = state.asserts;
    failures         = state.failures;
    allowed_failures = state.allowed_failures;

    auto& level = sections.levels[sections.depth - 1];

    ++level.current_section_id;
    if (level.current_section_id > level.max_section_id) {
        level.max_section_id = level.current_section_id;
    }

    if (sections.leaf_executed) {
        // We have already executed another leaf section; can't execute more
        // on this run, so don't bother going inside this one now.
        return false;
    }

    // Only enter this section if:
    //  - The section entered in the previous run was its immediate previous sibling, or
    //  - This section was already entered in the previous run, and child sections exist in it.
    if (level.current_section_id == level.previous_section_id + 1 ||
        (level.current_section_id == level.previous_section_id &&
         sections.depth < sections.levels.size())) {

        // First time entering this section, emit the section start event.
        if (level.current_section_id == level.previous_section_id + 1) {
            state.reg.report_callback(state.reg, event::section_started{data.id, data.location});
        }

        level.previous_section_id = level.current_section_id;
        sections.current_section.push_back(data);
        push_location(
            state, {data.location.file, data.location.line, location_type::section_scope});
        entered = true;
        return true;
    }

    return false;
}
} // namespace snitch::impl
