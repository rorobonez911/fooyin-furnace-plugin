/*
 * Furnace tracker module plugin for fooyin
 */

#include "furnaceplugin.h"
#include "furnaceinput.h"

using namespace Qt::StringLiterals;

namespace Fooyin::FurnacePlugin {

QString FurnaceInputPlugin::inputName() const
{
    return u"Furnace"_s;
}

InputCreator FurnaceInputPlugin::inputCreator() const
{
    InputCreator creator;
    creator.decoder = []() {
        return std::make_unique<FurnaceDecoder>();
    };
    creator.reader = []() {
        return std::make_unique<FurnaceReader>();
    };
    return creator;
}

} // namespace Fooyin::FurnacePlugin
