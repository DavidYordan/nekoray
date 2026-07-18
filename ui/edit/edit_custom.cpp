#include "edit_custom.h"
#include "ui_edit_custom.h"

#include "3rdparty/qt_compat/ui/widgets/editors/w_JsonEditor.hpp"
#include "fmt/CustomBean.hpp"

#include <QMessageBox>

EditCustom::EditCustom(QWidget *parent) : QWidget(parent), ui(new Ui::EditCustom) {
    ui->setupUi(this);
    ui->config_simple->setPlaceholderText(
        "example:\n"
        "{\n"
        "    \"type\": \"socks\",\n"
        "    \"server\": \"127.0.0.1\",\n"
        "    \"server_port\": 1080\n"
        "}\n");
}

EditCustom::~EditCustom() {
    delete ui;
}

#define SAVE_CUSTOM_BEAN               \
    P_SAVE_COMBO_STRING(core)          \
    P_SAVE_STRING_PLAIN(config_simple)

void EditCustom::onStart(std::shared_ptr<NekoGui::ProxyEntity> _ent) {
    this->ent = _ent;
    auto bean = this->ent->CustomBean();

    ui->core->addItems({"internal", "internal-full"});
    if (preset_core == "internal") {
        preset_config = "";
        ui->config_simple->setPlaceholderText(
            "{\n"
            "    \"type\": \"socks\",\n"
            "    // ...\n"
            "}");
    } else if (preset_core == "internal-full") {
        preset_config = "";
        ui->config_simple->setPlaceholderText(
            "{\n"
            "    \"inbounds\": [],\n"
            "    \"outbounds\": []\n"
            "}");
    }

    // load core ui
    P_LOAD_COMBO_STRING(core)
    ui->config_simple->setPlainText(bean->config_simple);

    // custom mode
    if (!bean->core.isEmpty()) {
        ui->core->setDisabled(true);
    } else if (!preset_core.isEmpty()) {
        bean->core = preset_core;
        ui->core->setDisabled(true);
        ui->core->setCurrentText(preset_core);
        ui->config_simple->setPlainText(preset_config);
    }

    const auto isSingBoxCustom = bean->core == "internal" || bean->core == "internal-full";
    const auto isPresetSingBoxCustom = preset_core == "internal" || preset_core == "internal-full";
    if (isSingBoxCustom || isPresetSingBoxCustom) {
        ui->core->hide();
        if (bean->core == "internal" || preset_core == "internal") {
            ui->core_l->setText(tr("Outbound JSON, please read the documentation."));
        } else {
            ui->core_l->setText(tr("Please fill the complete config."));
        }
    } else {
        ui->core_l->setText(tr("External custom cores are not supported."));
    }
}

bool EditCustom::onEnd() {
    if (get_edit_text_name().isEmpty()) {
        MessageBoxWarning(software_name, tr("Name cannot be empty."));
        return false;
    }
    if (ui->core->currentText().isEmpty()) {
        MessageBoxWarning(software_name, tr("Please pick a core."));
        return false;
    }
    if (ui->core->currentText() != "internal" && ui->core->currentText() != "internal-full") {
        MessageBoxWarning(software_name, tr("External custom cores are not supported."));
        return false;
    }

    auto bean = this->ent->CustomBean();

    SAVE_CUSTOM_BEAN

    return true;
}

void EditCustom::on_as_json_clicked() {
    auto editor = new JsonEditor(QString2QJsonObject(ui->config_simple->toPlainText()), this);
    auto result = editor->OpenEditor();
    if (!result.isEmpty()) {
        ui->config_simple->setPlainText(QJsonObject2QString(result, false));
    }
}
