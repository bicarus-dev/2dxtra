#include <meta.h>
#include <MinHook.h>
#include "../log.h"
#include "../game.h"
#include "../gui/gui.h"
#include "../chart_set.h"
#include "../score_set.h"
#include "../settings.h"
#include "reset_state_hook.h"
#include "score_invalidator_hook.h"
#include "../features/timing_histogram.h"

namespace iidxtra::reset_state_hook
{
	void* original_reset_state_fn = nullptr;

	auto reset_state_hook_fn(std::uint32_t a1) -> void*
	{
        timing_histogram::leave_result();

        // revert to default charts
        chart_set::revert();

        // mark session as valid
        score_invalidator_hook::reset_session();

		// clear scores
		score_set::custom.clear();

		score_set::clear_stock();

		log::debug("Cleared score data");

        // reset configuration
        if (gui::config_event_mode)
        {
            log::debug("Configuration reset");

            settings::reset(false);
        }

		return reinterpret_cast<void* (*) (std::uint32_t)>(original_reset_state_fn)(a1);
	}

	auto install_hook() -> void
		{ MH_CreateHook(bm2dx::addr->RESET_STATE_FN, reinterpret_cast<LPVOID>(reset_state_hook_fn), &original_reset_state_fn); }
}