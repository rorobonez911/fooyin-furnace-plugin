/*
 * Furnace tracker module plugin for fooyin
 */

#pragma once

#include <core/engine/audioinput.h>
#include <core/engine/inputplugin.h>
#include <core/plugins/plugin.h>

namespace Fooyin::FurnacePlugin {
class FurnaceInputPlugin : public QObject,
                           public Plugin,
                           public InputPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.fooyin.fooyin.plugin" FILE "furnaceinput.json")
    Q_INTERFACES(Fooyin::Plugin Fooyin::InputPlugin)

public:
    [[nodiscard]] QString inputName() const override;
    [[nodiscard]] InputCreator inputCreator() const override;
};
} // namespace Fooyin::FurnacePlugin
