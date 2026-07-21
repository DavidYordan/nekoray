#include "dialog_edit_group.h"
#include "ui_dialog_edit_group.h"

#include "db/Database.hpp"
#include "fmt/AnyTLSBean.hpp"
#include "ui/mainwindow_interface.h"

#include <QClipboard>
#include <QMessageBox>
#include <QSet>
#include <QUrl>

namespace {
    QString normalizeSourceType(const QString &value) {
        const auto lower = value.trimmed().toLower();
        if (lower == "auto") return {};
        if (QStringList{"clash", "subscription", "manual"}.contains(lower)) return lower;
        return {};
    }

    QStringList parseDohUpstreams(const QString &raw) {
        QString normalized = raw;
        normalized.replace(",", "\n");
        QStringList out;
        QSet<QString> seen;
        for (const auto &line: SplitLinesSkipSharp(normalized)) {
            const auto value = line.trimmed();
            if (value.isEmpty() || seen.contains(value)) continue;
            const QUrl url(value);
            if (!url.isValid() || url.scheme().toLower() != "https" || url.host().isEmpty() || url.path().isEmpty() || url.path() == "/") {
                continue;
            }
            seen.insert(value);
            out << value;
        }
        return out;
    }

    bool isVisibleAsciiClientValue(const QString &value) {
        if (value.isEmpty() || value.size() > 128) return false;
        for (const auto ch: value) {
            const auto code = ch.unicode();
            if (code < 0x21 || code > 0x7e) return false;
        }
        return true;
    }
} // namespace

#define ADJUST_SIZE runOnUiThread([=] { adjustSize(); adjustPosition(mainwindow); }, this);

DialogEditGroup::DialogEditGroup(const std::shared_ptr<NekoGui::Group> &ent, QWidget *parent) : QDialog(parent), ui(new Ui::DialogEditGroup) {
    ui->setupUi(this);
    this->ent = ent;

    connect(ui->type, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, [=](int index) {
        ui->cat_sub->setHidden(index == 0);
        ADJUST_SIZE
    });

    ui->name->setText(ent->name);
    ui->archive->setChecked(ent->archive);
    ui->skip_auto_update->setChecked(ent->skip_auto_update);
    ui->url->setText(ent->url);
    auto sourceType = ent->source_type.trimmed().isEmpty() ? "auto" : ent->source_type.trimmed().toLower();
    if (ui->source_type->findText(sourceType) < 0) sourceType = "auto";
    ui->source_type->setCurrentText(sourceType);
    ui->default_client_auto->setChecked(ent->DefaultClientManagedBySubscription());
    auto defaultClientMode = ent->default_client_mode.trimmed().isEmpty() ? "native" : ent->default_client_mode.trimmed().toLower();
    if (ui->default_client_mode->findText(defaultClientMode) < 0) defaultClientMode = "native";
    ui->default_client_mode->setCurrentText(defaultClientMode);
    ui->default_client_value->setText(ent->default_client_value);
    ui->default_server_resolver_auto->setChecked(ent->DefaultResolverManagedBySubscription());
    ui->default_server_resolver_doh->setPlainText(ent->default_server_resolver_doh);
    ui->default_server_resolver_fallback->setChecked(ent->default_server_resolver_allow_local_fallback);
    // Retain the serialized value for compatibility, but do not expose a
    // control that weakens strict provider DNS into local fallback.
    ui->default_server_resolver_fallback->setVisible(false);
    ui->type->setCurrentIndex(ent->url.isEmpty() ? 0 : 1);
    ui->type->currentIndexChanged(ui->type->currentIndex());
    ui->manually_column_width->setChecked(ent->manually_column_width);
    ui->cat_share->setVisible(false);

    auto refreshDefaultClientUi = [=] {
        const auto autoManaged = ui->default_client_auto->isChecked();
        const auto sourceType = normalizeSourceType(ui->source_type->currentText());
        ui->default_client_mode->setEnabled(!autoManaged);
        const auto mode = ui->default_client_mode->currentText();
        if (autoManaged && sourceType == "clash") {
            ui->default_client_mode->setCurrentText("mihomo");
            ui->default_client_value->setText("mihomo/1.19.28");
        } else if (autoManaged) {
            ui->default_client_mode->setCurrentText("native");
            ui->default_client_value->clear();
        } else if (mode == "mihomo") {
            ui->default_client_value->setText("mihomo/1.19.28");
        } else if (mode == "native") {
            ui->default_client_value->clear();
        }
        ui->default_client_value->setEnabled(!autoManaged && ui->default_client_mode->currentText() == "custom");
    };
    connect(ui->default_client_mode, &QComboBox::currentTextChanged, this, [=](const QString &) { refreshDefaultClientUi(); });
    connect(ui->default_client_auto, &QCheckBox::clicked, this, [=] { refreshDefaultClientUi(); });
    connect(ui->source_type, &QComboBox::currentTextChanged, this, [=](const QString &) { refreshDefaultClientUi(); });
    refreshDefaultClientUi();

    auto refreshDefaultResolverUi = [=] {
        const auto autoManaged = ui->default_server_resolver_auto->isChecked();
        ui->default_server_resolver_doh->setEnabled(!autoManaged);
        ui->default_server_resolver_fallback->setEnabled(!autoManaged);
    };
    connect(ui->default_server_resolver_auto, &QCheckBox::clicked, this, [=] { refreshDefaultResolverUi(); });
    refreshDefaultResolverUi();

    if (ent->id >= 0) { // already a group
        ui->type->setDisabled(true);
        if (!ent->Profiles().isEmpty()) {
            ui->cat_share->setVisible(true);
        }
    } else { // new group
        ui->front_proxy->hide();
        ui->front_proxy_l->hide();
        ui->front_proxy_clear->hide();
    }

    CACHE.front_proxy = ent->front_proxy_id;
    refresh_front_proxy();
    connect(ui->front_proxy_clear, &QPushButton::clicked, this, [=] {
        CACHE.front_proxy = -1;
        refresh_front_proxy();
    });

    connect(ui->copy_links, &QPushButton::clicked, this, [=] {
        QStringList links;
        for (const auto &[_, profile]: NekoGui::profileManager->profiles) {
            if (profile->gid != ent->id) continue;
            links += profile->bean->ToShareLink();
        }
        QApplication::clipboard()->setText(links.join("\n"));
        MessageBoxInfo(software_name, tr("Copied"));
    });
    connect(ui->copy_links_nkr, &QPushButton::clicked, this, [=] {
        QStringList links;
        for (const auto &[_, profile]: NekoGui::profileManager->profiles) {
            if (profile->gid != ent->id) continue;
            links += profile->bean->ToNekorayShareLink(profile->type);
        }
        QApplication::clipboard()->setText(links.join("\n"));
        MessageBoxInfo(software_name, tr("Copied"));
    });
    connect(ui->apply_client_to_profiles, &QPushButton::clicked, this, [=] { reset_profiles_to_inherit_defaults(true, false); });
    connect(ui->apply_resolver_to_profiles, &QPushButton::clicked, this, [=] { reset_profiles_to_inherit_defaults(false, true); });
    connect(ui->reset_profiles_inherit_defaults, &QPushButton::clicked, this, [=] { reset_profiles_to_inherit_defaults(true, true); });

    ADJUST_SIZE
}

DialogEditGroup::~DialogEditGroup() {
    delete ui;
}

bool DialogEditGroup::save_subscription_defaults_from_ui() {
    ent->source_type = normalizeSourceType(ui->source_type->currentText());
    const auto clientAuto = ui->default_client_auto->isChecked();
    ent->SetDefaultClientManagedBySubscription(clientAuto);
    if (clientAuto) {
        if (ent->source_type == "clash") {
            ent->default_client_mode = "mihomo";
            ent->default_client_value = "mihomo/1.19.28";
        } else {
            ent->default_client_mode.clear();
            ent->default_client_value.clear();
        }
    } else {
        ent->default_client_mode = ui->default_client_mode->currentText().trimmed().toLower();
        if (ent->default_client_mode == "native") {
            ent->default_client_mode.clear();
            ent->default_client_value.clear();
        } else if (ent->default_client_mode == "mihomo") {
            ent->default_client_value = "mihomo/1.19.28";
        } else if (ent->default_client_mode == "custom") {
            const auto clientValue = ui->default_client_value->text().trimmed();
            if (!isVisibleAsciiClientValue(clientValue)) {
                MessageBoxWarning(tr("Default Client"), tr("Custom client value must be 1..128 visible ASCII characters without spaces."));
                return false;
            }
            ent->default_client_value = clientValue;
        } else {
            ent->default_client_mode.clear();
            ent->default_client_value.clear();
        }
    }

    const auto resolverAuto = ui->default_server_resolver_auto->isChecked();
    ent->SetDefaultResolverManagedBySubscription(resolverAuto);
    if (!resolverAuto) {
        ent->default_server_resolver_doh = parseDohUpstreams(ui->default_server_resolver_doh->toPlainText()).join("\n");
    }
    return true;
}

void DialogEditGroup::accept() {
    if (ent->id >= 0) { // already a group
        if (!ent->url.isEmpty() && ui->url->text().isEmpty()) {
            MessageBoxWarning(tr("Warning"), tr("Please input URL"));
            return;
        }
    }
    ent->name = ui->name->text();
    ent->url = ui->url->text();
    ent->archive = ui->archive->isChecked();
    ent->skip_auto_update = ui->skip_auto_update->isChecked();
    if (!save_subscription_defaults_from_ui()) return;
    ent->manually_column_width = ui->manually_column_width->isChecked();
    ent->front_proxy_id = CACHE.front_proxy;
    QDialog::accept();
}

void DialogEditGroup::refresh_front_proxy() {
    auto fEnt = NekoGui::profileManager->GetProfile(CACHE.front_proxy);
    ui->front_proxy->setText(fEnt == nullptr ? tr("None") : fEnt->bean->DisplayTypeAndName());
}

void DialogEditGroup::reset_profiles_to_inherit_defaults(bool resetClient, bool resetResolver) {
    if (ent->id < 0 || (!resetClient && !resetResolver)) return;
    if (!save_subscription_defaults_from_ui()) return;
    ent->Save();

    const auto profiles = ent->Profiles();
    if (profiles.isEmpty()) return;
    int targetCount = 0;
    for (const auto &profile: profiles) {
        if (profile == nullptr || profile->bean == nullptr) continue;
        if (resetResolver || profile->type == "anytls") targetCount++;
    }
    if (targetCount == 0) {
        MessageBoxInfo(software_name, tr("No matching profiles."));
        return;
    }
    QString confirmation;
    if (resetClient && resetResolver) {
        confirmation = tr("Reset %1 profile(s) to inherit this group's default client and resolver?").arg(targetCount);
    } else if (resetClient) {
        confirmation = tr("Reset AnyTLS client on %1 profile(s) to inherit this group's default client?").arg(targetCount);
    } else {
        confirmation = tr("Reset server resolver on %1 profile(s) to inherit this group's default resolver?").arg(targetCount);
    }
    if (QMessageBox::question(this, tr("Confirmation"), confirmation)
        != QMessageBox::Yes) {
        return;
    }

    int changed = 0;
    for (const auto &profile: profiles) {
        if (profile == nullptr || profile->bean == nullptr) continue;
        bool profileChanged = false;
        if (resetClient && profile->type == "anytls") {
            profile->bean->inheritSubscriptionClient = true;
            auto anytls = profile->AnyTLSBean();
            anytls->anytlsClientMode = "native";
            anytls->anytlsClientValue.clear();
            profileChanged = true;
        }
        if (resetResolver) {
            profile->bean->inheritSubscriptionResolver = true;
            profile->bean->serverResolverDohUpstreams.clear();
            profileChanged = true;
        }
        if (!profileChanged) continue;
        profile->Save();
        changed++;
    }
    MessageBoxInfo(software_name, tr("Updated %1 profile(s).").arg(changed));
}

void DialogEditGroup::on_front_proxy_clicked() {
    auto parent = dynamic_cast<QWidget *>(this->parent());
    parent->hide();
    this->hide();
    GetMainWindow()->start_select_mode(this, [=](int id) {
        CACHE.front_proxy = id;
        refresh_front_proxy();
        parent->show();
        show();
    });
}
