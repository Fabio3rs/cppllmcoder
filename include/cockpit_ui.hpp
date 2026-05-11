#pragma once

#include "cockpit_runtime.hpp"

#include "agent.hpp"
#include "cockpit_consent.hpp"
#include "options.hpp"
#include "prompt_consent.hpp"
#include <memory>
#include <openai/openai.hpp>

struct CockpitAppDeps {
    app::Options options;
    RuntimeDefaults runtime;
    std::shared_ptr<Agent> agent;
    std::shared_ptr<CockpitConsentProvider> consent;
};

int runCockpitUi(CockpitAppDeps deps);
