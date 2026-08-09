#include "denoiser.hpp"

class DenoiserFactory : public famidec::FilterFactory {
public:
    famidec::IFilter* create() const override { return new famidec::Denoiser(); }
    const char* name() const override { return "denoise"; }
};

static bool denoiser_registered = ([]() {
    famidec::FilterRegistry::register_factory(new DenoiserFactory());
    return true;
})();
