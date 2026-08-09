#include "temporal.hpp"

class TemporalFilterFactory : public famidec::FilterFactory {
public:
    famidec::IFilter* create() const override { return new famidec::TemporalFilter(); }
    const char* name() const override { return "temporal"; }
};

static bool temporal_registered = ([]() {
    famidec::FilterRegistry::register_factory(new TemporalFilterFactory());
    return true;
})();
