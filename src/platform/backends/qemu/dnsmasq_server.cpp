/*
 * Copyright (C) 2018-2020 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "dnsmasq_server.h"
#include "dnsmasq_process_spec.h"

#include <multipass/format.h>
#include <multipass/logging/log.h>
#include <multipass/process/process.h>
#include <multipass/utils.h>
#include <shared/linux/process_factory.h>

#include <QDir>

#include <fstream>

namespace mp = multipass;
namespace mpl = multipass::logging;

namespace
{

auto make_dnsmasq_process(const mp::Path& data_dir, const QString& bridge_name, const std::string& subnet,
                          const QString& conf_file_path)
{
    auto process_spec = std::make_unique<mp::DNSMasqProcessSpec>(data_dir, bridge_name, subnet, conf_file_path);
    return MP_PROCFACTORY.create_process(std::move(process_spec));
}
} // namespace

mp::DNSMasqServer::DNSMasqServer(const Path& data_dir, const QString& bridge_name, const std::string& subnet)
    : data_dir{data_dir},
      bridge_name{bridge_name},
      subnet{subnet},
      conf_file{QDir(data_dir).absoluteFilePath("dnsmasq-XXXXXX.conf")}
{
    conf_file.open();
    conf_file.close();

    start_dnsmasq();
}

mp::DNSMasqServer::~DNSMasqServer()
{
    QObject::disconnect(finish_connection);

    mpl::log(mpl::Level::debug, "dnsmasq", "terminating");
    dnsmasq_cmd->terminate();

    if (!dnsmasq_cmd->wait_for_finished(1000))
    {
        mpl::log(mpl::Level::info, "dnsmasq", "failed to terminate nicely, killing");

        dnsmasq_cmd->kill();
        if (!dnsmasq_cmd->wait_for_finished(100))
            mpl::log(mpl::Level::warning, "dnsmasq", "failed to kill");
    }
}

mp::optional<mp::IPAddress> mp::DNSMasqServer::get_ip_for(const std::string& hw_addr)
{
    // DNSMasq leases entries consist of:
    // <lease expiration> <mac addr> <ipv4> <name> * * *
    const auto path = QDir(data_dir).filePath("dnsmasq.leases").toStdString();
    const std::string delimiter{" "};
    const int hw_addr_idx{1};
    const int ipv4_idx{2};
    std::ifstream leases_file{path};
    std::string line;
    while (getline(leases_file, line))
    {
        const auto fields = mp::utils::split(line, delimiter);
        if (fields.size() > 2 && fields[hw_addr_idx] == hw_addr)
            return mp::optional<mp::IPAddress>{fields[ipv4_idx]};
    }
    return mp::nullopt;
}

void mp::DNSMasqServer::release_mac(const std::string& hw_addr)
{
    auto ip = get_ip_for(hw_addr);
    if (!ip)
    {
        mpl::log(mpl::Level::warning, "dnsmasq", fmt::format("attempting to release non-existant addr: {}", hw_addr));
        return;
    }

    QProcess dhcp_release;
    QObject::connect(&dhcp_release, &QProcess::errorOccurred, [&ip, &hw_addr](QProcess::ProcessError error) {
        mpl::log(mpl::Level::warning, "dnsmasq",
                 fmt::format("failed to release ip addr {} with mac {}: {}", ip.value().as_string(), hw_addr,
                             utils::qenum_to_string(error)));
    });

    auto log_exit_status = [&ip, &hw_addr](int exit_code, QProcess::ExitStatus exit_status) {
        if (exit_code == 0 && exit_status == QProcess::NormalExit)
            return;

        auto msg = fmt::format("failed to release ip addr {} with mac {}, exit_code: {}", ip.value().as_string(),
                               hw_addr, exit_code);
        mpl::log(mpl::Level::warning, "dnsmasq", msg);
    };
    QObject::connect(&dhcp_release, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
                     log_exit_status);

    dhcp_release.start("dhcp_release", QStringList() << bridge_name << QString::fromStdString(ip.value().as_string())
                                                     << QString::fromStdString(hw_addr));

    dhcp_release.waitForFinished();
}

void mp::DNSMasqServer::check_dnsmasq_running()
{
    if (!dnsmasq_cmd->running())
    {
        mpl::log(mpl::Level::warning, "dnsmasq", "Not running");
        start_dnsmasq();
    }
}

void mp::DNSMasqServer::start_dnsmasq()
{
    mpl::log(mpl::Level::debug, "dnsmasq", "Starting dnsmasq");

    dnsmasq_cmd = make_dnsmasq_process(data_dir, bridge_name, subnet, conf_file.fileName());
    dnsmasq_cmd->start();
    if (!dnsmasq_cmd->wait_for_started())
    {
        auto err_msg = std::string{"Multipass dnsmasq failed to start"};
        if (auto err_detail = dnsmasq_cmd->process_state().failure_message(); !err_detail.isEmpty())
            err_msg += fmt::format(": {}", err_detail);

        dnsmasq_cmd->kill();
        throw std::runtime_error(err_msg);
    }

    finish_connection = QObject::connect(dnsmasq_cmd.get(), &mp::Process::finished, [](ProcessState process_state) {
        auto err_msg = std::string{"died"};
        if (auto err_detail = process_state.failure_message(); !err_detail.isEmpty())
            err_msg += fmt::format(": {}", err_detail);
        if (process_state.exit_code == 2)
            err_msg += ". Ensure nothing is using port 53.";

        mpl::log(mpl::Level::error, "dnsmasq", err_msg);
    });
}
