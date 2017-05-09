/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "runconfiguration.h"

#include "project.h"
#include "target.h"
#include "toolchain.h"
#include "abi.h"
#include "buildconfiguration.h"
#include "environmentaspect.h"
#include "kitinformation.h"
#include "runnables.h"

#include <extensionsystem/pluginmanager.h>

#include <utils/algorithm.h>
#include <utils/checkablemessagebox.h>
#include <utils/outputformatter.h>
#include <utils/qtcassert.h>
#include <utils/utilsicons.h>

#include <coreplugin/icore.h>
#include <coreplugin/icontext.h>

#include <QDir>
#include <QPushButton>
#include <QTimer>

#ifdef Q_OS_OSX
#include <ApplicationServices/ApplicationServices.h>
#endif

#if defined (WITH_JOURNALD)
#include "journaldwatcher.h"
#endif

using namespace Utils;
using namespace ProjectExplorer::Internal;

const bool debugStates = false;

namespace ProjectExplorer {

///////////////////////////////////////////////////////////////////////
//
// ISettingsAspect
//
///////////////////////////////////////////////////////////////////////

ISettingsAspect *ISettingsAspect::clone() const
{
    ISettingsAspect *other = create();
    QVariantMap data;
    toMap(data);
    other->fromMap(data);
    return other;
}

///////////////////////////////////////////////////////////////////////
//
// IRunConfigurationAspect
//
///////////////////////////////////////////////////////////////////////

IRunConfigurationAspect::IRunConfigurationAspect(RunConfiguration *runConfig) :
    m_runConfiguration(runConfig)
{ }

IRunConfigurationAspect::~IRunConfigurationAspect()
{
    delete m_projectSettings;
}

/*!
    Returns the widget used to configure this run configuration. Ownership is
    transferred to the caller.
*/

RunConfigWidget *IRunConfigurationAspect::createConfigurationWidget() const
{
    return m_runConfigWidgetCreator ? m_runConfigWidgetCreator() : nullptr;
}

void IRunConfigurationAspect::setProjectSettings(ISettingsAspect *settings)
{
    m_projectSettings = settings;
}

void IRunConfigurationAspect::setGlobalSettings(ISettingsAspect *settings)
{
    m_globalSettings = settings;
}

void IRunConfigurationAspect::setUsingGlobalSettings(bool value)
{
    m_useGlobalSettings = value;
}

ISettingsAspect *IRunConfigurationAspect::currentSettings() const
{
   return m_useGlobalSettings ? m_globalSettings : m_projectSettings;
}

void IRunConfigurationAspect::fromMap(const QVariantMap &map)
{
    m_projectSettings->fromMap(map);
    m_useGlobalSettings = map.value(m_id.toString() + QLatin1String(".UseGlobalSettings"), true).toBool();
}

void IRunConfigurationAspect::toMap(QVariantMap &map) const
{
    m_projectSettings->toMap(map);
    map.insert(m_id.toString() + QLatin1String(".UseGlobalSettings"), m_useGlobalSettings);
}

void IRunConfigurationAspect::setRunConfigWidgetCreator(const RunConfigWidgetCreator &runConfigWidgetCreator)
{
    m_runConfigWidgetCreator = runConfigWidgetCreator;
}

IRunConfigurationAspect *IRunConfigurationAspect::clone(RunConfiguration *runConfig) const
{
    IRunConfigurationAspect *other = create(runConfig);
    if (m_projectSettings)
        other->m_projectSettings = m_projectSettings->clone();
    other->m_globalSettings = m_globalSettings;
    other->m_useGlobalSettings = m_useGlobalSettings;
    return other;
}

void IRunConfigurationAspect::resetProjectToGlobalSettings()
{
    QTC_ASSERT(m_globalSettings, return);
    QVariantMap map;
    m_globalSettings->toMap(map);
    m_projectSettings->fromMap(map);
}


/*!
    \class ProjectExplorer::RunConfiguration
    \brief The RunConfiguration class is the base class for a run configuration.

    A run configuration specifies how a target should be run, while a runner
    does the actual running.

    All RunControls and the target hold a shared pointer to the run
    configuration. That is, the lifetime of the run configuration might exceed
    the life of the target.
    The user might still have a RunControl running (or output tab of that RunControl open)
    and yet unloaded the target.

    Also, a run configuration might be already removed from the list of run
    configurations
    for a target, but still be runnable via the output tab.
*/

RunConfiguration::RunConfiguration(Target *target, Core::Id id) :
    ProjectConfiguration(target, id)
{
    Q_ASSERT(target);
    ctor();

    addExtraAspects();
}

RunConfiguration::RunConfiguration(Target *target, RunConfiguration *source) :
    ProjectConfiguration(target, source)
{
    Q_ASSERT(target);
    ctor();
    foreach (IRunConfigurationAspect *aspect, source->m_aspects) {
        IRunConfigurationAspect *clone = aspect->clone(this);
        if (clone)
            m_aspects.append(clone);
    }
}

RunConfiguration::~RunConfiguration()
{
    qDeleteAll(m_aspects);
}

void RunConfiguration::addExtraAspects()
{
    foreach (IRunControlFactory *factory, ExtensionSystem::PluginManager::getObjects<IRunControlFactory>())
        addExtraAspect(factory->createRunConfigurationAspect(this));
}

void RunConfiguration::addExtraAspect(IRunConfigurationAspect *aspect)
{
    if (aspect)
        m_aspects += aspect;
}

void RunConfiguration::ctor()
{
    connect(this, &RunConfiguration::enabledChanged,
            this, &RunConfiguration::requestRunActionsUpdate);

    Utils::MacroExpander *expander = macroExpander();
    expander->setDisplayName(tr("Run Settings"));
    expander->setAccumulating(true);
    expander->registerSubProvider([this]() -> Utils::MacroExpander * {
        BuildConfiguration *bc = target()->activeBuildConfiguration();
        return bc ? bc->macroExpander() : target()->macroExpander();
    });
    expander->registerPrefix("CurrentRun:Env", tr("Variables in the current run environment"),
                             [this](const QString &var) {
        const auto envAspect = extraAspect<EnvironmentAspect>();
        return envAspect ? envAspect->environment().value(var) : QString();
    });
    expander->registerVariable(Constants::VAR_CURRENTRUN_NAME,
            QCoreApplication::translate("ProjectExplorer", "The currently active run configuration's name."),
            [this] { return displayName(); }, false);
}

/*!
    Checks whether a run configuration is enabled.
*/

bool RunConfiguration::isEnabled() const
{
    return true;
}

QString RunConfiguration::disabledReason() const
{
    return QString();
}

bool RunConfiguration::isConfigured() const
{
    return true;
}

RunConfiguration::ConfigurationState RunConfiguration::ensureConfigured(QString *errorMessage)
{
    if (isConfigured())
        return Configured;
    if (errorMessage)
        *errorMessage = tr("Unknown error.");
    return UnConfigured;
}


BuildConfiguration *RunConfiguration::activeBuildConfiguration() const
{
    if (!target())
        return nullptr;
    return target()->activeBuildConfiguration();
}

Target *RunConfiguration::target() const
{
    return static_cast<Target *>(parent());
}

QVariantMap RunConfiguration::toMap() const
{
    QVariantMap map = ProjectConfiguration::toMap();

    foreach (IRunConfigurationAspect *aspect, m_aspects)
        aspect->toMap(map);

    return map;
}

Abi RunConfiguration::abi() const
{
    BuildConfiguration *bc = target()->activeBuildConfiguration();
    if (!bc)
        return Abi::hostAbi();
    ToolChain *tc = ToolChainKitInformation::toolChain(target()->kit(), Constants::CXX_LANGUAGE_ID);
    if (!tc)
        return Abi::hostAbi();
    return tc->targetAbi();
}

bool RunConfiguration::fromMap(const QVariantMap &map)
{
    foreach (IRunConfigurationAspect *aspect, m_aspects)
        aspect->fromMap(map);

    return ProjectConfiguration::fromMap(map);
}

/*!
    \class ProjectExplorer::IRunConfigurationAspect

    \brief The IRunConfigurationAspect class provides an additional
    configuration aspect.

    Aspects are a mechanism to add RunControl-specific options to a run
    configuration without subclassing the run configuration for every addition.
    This prevents a combinatorial explosion of subclasses and eliminates
    the need to add all options to the base class.
*/

/*!
    Returns extra aspects.

    \sa ProjectExplorer::IRunConfigurationAspect
*/

QList<IRunConfigurationAspect *> RunConfiguration::extraAspects() const
{
    return m_aspects;
}

IRunConfigurationAspect *RunConfiguration::extraAspect(Core::Id id) const
{
    return Utils::findOrDefault(m_aspects, Utils::equal(&IRunConfigurationAspect::id, id));
}

/*!
    \internal

    \class ProjectExplorer::Runnable

    \brief The ProjectExplorer::Runnable class wraps information needed
    to execute a process on a target device.

    A target specific \l RunConfiguration implementation can specify
    what information it considers necessary to execute a process
    on the target. Target specific) \n IRunControlFactory implementation
    can use that information either unmodified or tweak it or ignore
    it when setting up a RunControl.

    From Qt Creator's core perspective a Runnable object is opaque.
*/

/*!
    \internal

    \brief Returns a \l Runnable described by this RunConfiguration.
*/

Runnable RunConfiguration::runnable() const
{
    return Runnable();
}

Utils::OutputFormatter *RunConfiguration::createOutputFormatter() const
{
    return new Utils::OutputFormatter();
}


/*!
    \class ProjectExplorer::IRunConfigurationFactory

    \brief The IRunConfigurationFactory class restores run configurations from
    settings.

    The run configuration factory is used for restoring run configurations from
    settings and for creating new run configurations in the \gui {Run Settings}
    dialog.
    To restore run configurations, use the
    \c {bool canRestore(Target *parent, const QString &id)}
    and \c {RunConfiguration* create(Target *parent, const QString &id)}
    functions.

    To generate a list of creatable run configurations, use the
    \c {QStringList availableCreationIds(Target *parent)} and
    \c {QString displayNameForType(const QString&)} functions. To create a
    run configuration, use \c create().
*/

/*!
    \fn QStringList ProjectExplorer::IRunConfigurationFactory::availableCreationIds(Target *parent) const

    Shows the list of possible additions to a target. Returns a list of types.
*/

/*!
    \fn QString ProjectExplorer::IRunConfigurationFactory::displayNameForId(Core::Id id) const
    Translates the types to names to display to the user.
*/

IRunConfigurationFactory::IRunConfigurationFactory(QObject *parent) :
    QObject(parent)
{
}

RunConfiguration *IRunConfigurationFactory::create(Target *parent, Core::Id id)
{
    if (!canCreate(parent, id))
        return nullptr;
    RunConfiguration *rc = doCreate(parent, id);
    if (!rc)
        return nullptr;
    return rc;
}

RunConfiguration *IRunConfigurationFactory::restore(Target *parent, const QVariantMap &map)
{
    if (!canRestore(parent, map))
        return nullptr;
    RunConfiguration *rc = doRestore(parent, map);
    if (!rc->fromMap(map)) {
        delete rc;
        rc = nullptr;
    }
    return rc;
}

IRunConfigurationFactory *IRunConfigurationFactory::find(Target *parent, const QVariantMap &map)
{
    return ExtensionSystem::PluginManager::getObject<IRunConfigurationFactory>(
        [&parent, &map](IRunConfigurationFactory *factory) {
            return factory->canRestore(parent, map);
        });
}

IRunConfigurationFactory *IRunConfigurationFactory::find(Target *parent, RunConfiguration *rc)
{
    return ExtensionSystem::PluginManager::getObject<IRunConfigurationFactory>(
        [&parent, rc](IRunConfigurationFactory *factory) {
            return factory->canClone(parent, rc);
        });
}

QList<IRunConfigurationFactory *> IRunConfigurationFactory::find(Target *parent)
{
    return ExtensionSystem::PluginManager::getObjects<IRunConfigurationFactory>(
        [&parent](IRunConfigurationFactory *factory) {
            return !factory->availableCreationIds(parent).isEmpty();
        });
}

/*!
    \class ProjectExplorer::IRunControlFactory

    \brief The IRunControlFactory class creates RunControl objects matching a
    run configuration.
*/

/*!
    \fn RunConfigWidget *ProjectExplorer::IRunConfigurationAspect::createConfigurationWidget()

    Returns a widget used to configure this runner. Ownership is transferred to
    the caller.

    Returns null if @p \a runConfiguration is not suitable for RunControls from this
    factory, or no user-accessible
    configuration is required.
*/

IRunControlFactory::IRunControlFactory(QObject *parent)
    : QObject(parent)
{
}

/*!
    Returns an IRunConfigurationAspect to carry options for RunControls this
    factory can create.

    If no extra options are required, it is allowed to return null like the
    default implementation does. This function is intended to be called from the
    RunConfiguration constructor, so passing a RunConfiguration pointer makes
    no sense because that object is under construction at the time.
*/

IRunConfigurationAspect *IRunControlFactory::createRunConfigurationAspect(RunConfiguration *rc)
{
    Q_UNUSED(rc);
    return nullptr;
}

/*!
    \class ProjectExplorer::RunControl
    \brief The RunControl class instances represent one item that is run.
*/

/*!
    \fn QIcon ProjectExplorer::RunControl::icon() const
    Returns the icon to be shown in the Outputwindow.

    TODO the icon differs currently only per "mode", so this is more flexible
    than it needs to be.
*/

namespace Internal {

class RunControlPrivate : public QObject
{
public:
    RunControlPrivate(RunControl *parent, RunConfiguration *runConfiguration, Core::Id mode)
        : q(parent), runMode(mode), runConfiguration(runConfiguration)
    {
        icon = Icons::RUN_SMALL_TOOLBAR;
        if (runConfiguration) {
            runnable = runConfiguration->runnable();
            displayName  = runConfiguration->displayName();
            outputFormatter = runConfiguration->createOutputFormatter();
            device = DeviceKitInformation::device(runConfiguration->target()->kit());
            project = runConfiguration->target()->project();
        }
    }

    ~RunControlPrivate()
    {
        QTC_CHECK(state == State::Stopped || state == State::Initialized);
        delete targetRunner;
        delete toolRunner;
        delete outputFormatter;
    }

    enum class State {
        Initialized,      // Default value after creation.
        TargetPreparing,  // initiateStart() was called, target boots up, connects, etc
        ToolPreparing,    // Target is acessible, tool boots
        TargetStarting,   // Late corrections on the target side after tool is available.
        ToolStarting,     // Actual process/tool starts.
        Running,          // All good and running.
        ToolStopping,     // initiateStop() was called, stop application/tool
        TargetStopping,   // Potential clean up on target, set idle state, etc.
        Stopped,          // all good, but stopped. Can possibly be re-started
    };
    Q_ENUM(State)

    void checkState(State expectedState);
    void setState(State state);

    void debugMessage(const QString &msg);
    QString stateName(State s) const;

    void initiateStart();

    void onTargetPrepared();
    void onTargetPrepareFailed(const QString &msg);

    void onToolPrepared();
    void onToolPrepareFailed(const QString &msg);

    void onTargetStarted();
    void onTargetStartFailed(const QString &msg);

    void onToolStarted();
    void onToolStartFailed(const QString &msg);

    void initiateStop();
    void onToolStopped();
    void onToolStopFailed(const QString &msg);

    void onTargetStopped();
    void onTargetStopFailed(const QString &msg);

    void onToolFailed(const QString &msg);
    void onToolSuccess();

    void onTargetFailed(const QString &msg);
    void onTargetSuccess();

    void handleFailure();

    void showError(const QString &msg);

    static bool isAllowedTransition(State from, State to);

    RunControl *q;
    QString displayName;
    Runnable runnable;
    IDevice::ConstPtr device;
    Connection connection;
    Core::Id runMode;
    Utils::Icon icon;
    const QPointer<RunConfiguration> runConfiguration; // Not owned.
    QPointer<Project> project; // Not owned.
    QPointer<TargetRunner> targetRunner; // Owned. QPointer as "extra safety" for now.
    QPointer<ToolRunner> toolRunner; // Owned. QPointer as "extra safety" for now.
    Utils::OutputFormatter *outputFormatter = nullptr;
    std::function<bool(bool*)> promptToStop;

    // A handle to the actual application process.
    Utils::ProcessHandle applicationProcessHandle;

    State state = State::Initialized;
    bool supportsReRunning = true;

#ifdef Q_OS_OSX
    // This is used to bring apps in the foreground on Mac
    int foregroundCount;
#endif
};

} // Internal

using namespace Internal;

RunControl::RunControl(RunConfiguration *runConfiguration, Core::Id mode) :
    d(new RunControlPrivate(this, runConfiguration, mode))
{
    (void) new TargetRunner(this);
    (void) new ToolRunner(this);
#ifdef WITH_JOURNALD
    JournaldWatcher::instance()->subscribe(this, [this](const JournaldWatcher::LogEntry &entry) {
        if (entry.value("_MACHINE_ID") != JournaldWatcher::instance()->machineId())
            return;

        const QByteArray pid = entry.value("_PID");
        if (pid.isEmpty())
            return;

        const qint64 pidNum = static_cast<qint64>(QString::fromLatin1(pid).toInt());
        if (pidNum != d->applicationProcessHandle.pid())
            return;

        const QString message = QString::fromUtf8(entry.value("MESSAGE")) + "\n";
        appendMessageRequested(this, message, Utils::OutputFormat::LogMessageFormat);
    });
#endif
}

RunControl::~RunControl()
{
#ifdef WITH_JOURNALD
    JournaldWatcher::instance()->unsubscribe(this);
#endif
    disconnect();
    delete d;
    d = nullptr;
}

void RunControl::initiateStart()
{
    emit aboutToStart();
    start();
}

void RunControl::start()
{
    d->initiateStart();
}

void RunControl::initiateStop()
{
    stop();
}

void RunControl::stop()
{
    d->initiateStop();
}

void RunControlPrivate::initiateStart()
{
    checkState(State::Initialized);
    setState(State::TargetPreparing);
    debugMessage("Queue: Prepare target runner");
    QTimer::singleShot(0, targetRunner, [this](){ targetRunner->prepare(); });
}

void RunControlPrivate::onTargetPrepared()
{
    checkState(State::TargetPreparing);
    setState(State::ToolPreparing);
    debugMessage("Queue: Prepare tool runner");
    QTimer::singleShot(0, toolRunner, [this](){ toolRunner->prepare(); });
}

void RunControlPrivate::onTargetPrepareFailed(const QString &msg)
{
    checkState(State::TargetPreparing);
    toolRunner->onTargetFailure();
    showError(msg);
    setState(State::Stopped);
}

void RunControlPrivate::onToolPrepared()
{
    checkState(State::ToolPreparing);
    setState(State::TargetStarting);
    debugMessage("Queue: Start target runner");
    QTimer::singleShot(0, targetRunner, [this](){ targetRunner->start(); });
}

void RunControlPrivate::onToolPrepareFailed(const QString &msg)
{
    checkState(State::ToolPreparing);
    targetRunner->onToolFailure();
    showError(msg);
    setState(State::Stopped);
}

void RunControlPrivate::onTargetStarted()
{
    checkState(State::TargetStarting);
    setState(State::ToolStarting);
    debugMessage("Queue: Start tool runner");
    QTimer::singleShot(0, toolRunner, [this](){ toolRunner->start(); });
}

void RunControlPrivate::onTargetStartFailed(const QString &msg)
{
    checkState(State::TargetStarting);
    toolRunner->onTargetFailure();
    showError(msg);
    setState(State::Stopped);
}

void RunControlPrivate::onToolStarted()
{
    checkState(State::ToolStarting);
    setState(State::Running);
}

void RunControlPrivate::onToolStartFailed(const QString &msg)
{
    checkState(State::ToolStarting);
    targetRunner->onToolFailure();
    showError(msg);
    setState(State::Stopped);
}

void RunControlPrivate::initiateStop()
{
    checkState(State::Running);
    setState(State::ToolStopping);
    debugMessage("Queue: Stop tool runner");
    QTimer::singleShot(0, toolRunner, [this](){ toolRunner->stop(); });
}

void RunControlPrivate::onToolStopped()
{
    toolRunner->onStop();
    debugMessage("Tool stopped");
    checkState(State::ToolStopping);
    setState(State::TargetStopping);
    debugMessage("Queue: Stop target runner");
    QTimer::singleShot(0, targetRunner, [this](){ targetRunner->stop(); });
}

void RunControlPrivate::onToolStopFailed(const QString &msg)
{
    checkState(State::ToolStopping);
    targetRunner->onToolFailure();
    debugMessage("Tool stop failed");
    showError(msg);
    setState(State::Stopped);
}

void RunControlPrivate::onTargetStopped()
{
    targetRunner->onStop();
    debugMessage("Target stopped");
    checkState(State::TargetStopping);
    setState(State::Stopped);
}

void RunControlPrivate::onTargetStopFailed(const QString &msg)
{
    debugMessage("Target stop failed");
    checkState(State::TargetStopping);
    toolRunner->onTargetFailure();
    showError(msg);
    setState(State::Stopped);
}

void RunControlPrivate::onTargetFailed(const QString &msg)
{
    debugMessage("Target operation failed");
    if (state == State::TargetPreparing) {
        onTargetPrepareFailed(msg);
    } else if (state == State::TargetStarting) {
        onTargetStartFailed(msg);
    } else if (state == State::TargetStopping) {
        onTargetStopFailed(msg);
    } else {
        showError(msg);
        showError(RunControl::tr("Unexpected state: %1").arg(int(state)));
        setState(State::Stopped);
    }
}

void RunControlPrivate::onTargetSuccess()
{
    debugMessage("Target operation successful");
    if (state == State::TargetPreparing) {
        onTargetPrepared();
    } else if (state == State::TargetStarting) {
        onTargetStarted();
    } else if (state == State::TargetStopping) {
        onTargetStopped();
    } else {
        showError(RunControl::tr("Unexpected state: %1").arg(int(state)));
        setState(State::Stopped);
    }
}

void RunControlPrivate::onToolFailed(const QString &msg)
{
    debugMessage("Tool operation failed");
    if (state == State::ToolPreparing) {
        onToolPrepareFailed(msg);
    } else if (state == State::ToolStarting) {
        onToolStartFailed(msg);
    } else if (state == State::ToolStopping) {
        onToolStartFailed(msg);
    } else {
        showError(msg);
        showError(RunControl::tr("Unexpected state: %1").arg(int(state)));
        setState(State::Stopped);
    }
}

void RunControlPrivate::onToolSuccess()
{
    debugMessage("Tool operation successful");
    if (state == State::ToolPreparing) {
        onToolPrepared();
    } else if (state == State::ToolStarting) {
        onToolStarted();
    } else if (state == State::ToolStopping) {
        onToolStopped();
    } else {
        showError(RunControl::tr("Unexpected state: %1").arg(int(state)));
        setState(State::Stopped);
    }
}


void RunControlPrivate::handleFailure()
{
    switch (state) {
    case State::Initialized:
    case State::TargetPreparing:
    case State::ToolPreparing:
    case State::TargetStarting:
    case State::ToolStarting:
    case State::Running:
    case State::ToolStopping:
    case State::TargetStopping:
    case State::Stopped:
        setState(State::Stopped);
        break;
    }
}

void RunControlPrivate::showError(const QString &msg)
{
    if (!msg.isEmpty())
        q->appendMessage(msg, ErrorMessageFormat);
}

Utils::OutputFormatter *RunControl::outputFormatter() const
{
    return d->outputFormatter;
}

Core::Id RunControl::runMode() const
{
    return d->runMode;
}

const Runnable &RunControl::runnable() const
{
    return d->runnable;
}

void RunControl::setRunnable(const Runnable &runnable)
{
    d->runnable = runnable;
}

const Connection &RunControl::connection() const
{
    return d->connection;
}

void RunControl::setConnection(const Connection &connection)
{
    d->connection = connection;
}

ToolRunner *RunControl::toolRunner() const
{
    return d->toolRunner;
}

void RunControl::setToolRunner(ToolRunner *tool)
{
    delete d->toolRunner;
    d->toolRunner = tool;
}

TargetRunner *RunControl::targetRunner() const
{
    return d->targetRunner;
}

void RunControl::setTargetRunner(TargetRunner *runner)
{
    delete d->targetRunner;
    d->targetRunner = runner;
}

QString RunControl::displayName() const
{
    return d->displayName;
}

void RunControl::setDisplayName(const QString &displayName)
{
    d->displayName = displayName;
}

void RunControl::setIcon(const Utils::Icon &icon)
{
    d->icon = icon;
}

Utils::Icon RunControl::icon() const
{
    return d->icon;
}

Abi RunControl::abi() const
{
    if (const RunConfiguration *rc = d->runConfiguration.data())
        return rc->abi();
    return Abi();
}

IDevice::ConstPtr RunControl::device() const
{
   return d->device;
}

RunConfiguration *RunControl::runConfiguration() const
{
    return d->runConfiguration.data();
}

Project *RunControl::project() const
{
    return d->project.data();
}

bool RunControl::canReUseOutputPane(const RunControl *other) const
{
    if (other->isRunning())
        return false;

    return d->runnable.canReUseOutputPane(other->d->runnable);
}

/*!
    A handle to the application process.

    This is typically a process id, but should be treated as
    opaque handle to the process controled by this \c RunControl.
*/

ProcessHandle RunControl::applicationProcessHandle() const
{
    return d->applicationProcessHandle;
}

void RunControl::setApplicationProcessHandle(const ProcessHandle &handle)
{
    if (d->applicationProcessHandle != handle) {
        d->applicationProcessHandle = handle;
        emit applicationProcessHandleChanged(QPrivateSignal());
    }
}

/*!
    Prompts to stop. If \a optionalPrompt is passed, a \gui {Do not ask again}
    checkbox is displayed and the result is returned in \a *optionalPrompt.
*/

bool RunControl::promptToStop(bool *optionalPrompt) const
{
    QTC_ASSERT(isRunning(), return true);
    if (optionalPrompt && !*optionalPrompt)
        return true;

    // Overridden.
    if (d->promptToStop)
        return d->promptToStop(optionalPrompt);

    const QString msg = tr("<html><head/><body><center><i>%1</i> is still running.<center/>"
                           "<center>Force it to quit?</center></body></html>").arg(displayName());
    return showPromptToStopDialog(tr("Application Still Running"), msg,
                                  tr("Force &Quit"), tr("&Keep Running"),
                                  optionalPrompt);
}

void RunControl::setPromptToStop(const std::function<bool (bool *)> &promptToStop)
{
    d->promptToStop = promptToStop;
}

bool RunControl::supportsReRunning() const
{
    return d->supportsReRunning;
}

void RunControl::setSupportsReRunning(bool reRunningSupported)
{
    d->supportsReRunning = reRunningSupported;
}

bool RunControl::isRunning() const
{
    return d->state == RunControlPrivate::State::Running;
}

/*!
    Prompts to terminate the application with the \gui {Do not ask again}
    checkbox.
*/

bool RunControl::showPromptToStopDialog(const QString &title,
                                        const QString &text,
                                        const QString &stopButtonText,
                                        const QString &cancelButtonText,
                                        bool *prompt)
{
    // Show a question message box where user can uncheck this
    // question for this class.
    Utils::CheckableMessageBox messageBox(Core::ICore::mainWindow());
    messageBox.setWindowTitle(title);
    messageBox.setText(text);
    messageBox.setStandardButtons(QDialogButtonBox::Yes|QDialogButtonBox::Cancel);
    if (!stopButtonText.isEmpty())
        messageBox.button(QDialogButtonBox::Yes)->setText(stopButtonText);
    if (!cancelButtonText.isEmpty())
        messageBox.button(QDialogButtonBox::Cancel)->setText(cancelButtonText);
    messageBox.setDefaultButton(QDialogButtonBox::Yes);
    if (prompt) {
        messageBox.setCheckBoxText(Utils::CheckableMessageBox::msgDoNotAskAgain());
        messageBox.setChecked(false);
    } else {
        messageBox.setCheckBoxVisible(false);
    }
    messageBox.exec();
    const bool close = messageBox.clickedStandardButton() == QDialogButtonBox::Yes;
    if (close && prompt && messageBox.isChecked())
        *prompt = false;
    return close;
}

bool RunControlPrivate::isAllowedTransition(State from, State to)
{
    switch (from) {
    case State::Initialized:
        return to == State::TargetPreparing;
    case State::TargetPreparing:
        return to == State::ToolPreparing;
    case State::ToolPreparing:
        return to == State::TargetStarting;
    case State::TargetStarting:
        return to == State::ToolStarting;
    case State::ToolStarting:
        return to == State::Running;
    case State::Running:
        return to == State::ToolStopping
            || to == State::Stopped;
    case State::ToolStopping:
        return to == State::TargetStopping;
    case State::TargetStopping:
        return to == State::Stopped;
    case State::Stopped:
        return false;
    }
    qDebug() << "UNKNOWN DEBUGGER STATE:" << from;
    return false;
}

void RunControlPrivate::checkState(State expectedState)
{
    if (state != expectedState)
        qDebug() << "Unexpected state " << expectedState << " have: " << state;
}

QString RunControlPrivate::stateName(State s) const
{
#    define SN(x) case x: return QLatin1String(#x);
    switch (s) {
        SN(State::Initialized)
        SN(State::TargetPreparing)
        SN(State::ToolPreparing)
        SN(State::TargetStarting)
        SN(State::ToolStarting)
        SN(State::Running)
        SN(State::ToolStopping)
        SN(State::TargetStopping)
        SN(State::Stopped)
    }
    return QLatin1String("<unknown>");
#    undef SN
}

void RunControlPrivate::setState(State newState)
{
    if (!isAllowedTransition(state, newState))
        qDebug() << "Invalid run state transition from " << state << " to " << newState;

    state = newState;

    debugMessage("Entering state " + stateName(newState));

    // Extra reporting.
    switch (state) {
    case State::Running:
        emit q->started();
        break;
    case State::Stopped:
        q->setApplicationProcessHandle(Utils::ProcessHandle());
        toolRunner->onFinished();
        targetRunner->onFinished();
        state = State::Initialized; // Reset for potential re-running.
        emit q->finished();
        break;
    default:
        break;
    }
}

void RunControlPrivate::debugMessage(const QString &msg)
{
    if (debugStates)
        q->appendMessage(msg + '\n', Utils::DebugFormat);
}

/*!
    Brings the application determined by this RunControl's \c applicationProcessHandle
    to the foreground.

    The default implementation raises the application on Mac, and does
    nothing elsewhere.
*/
void RunControl::bringApplicationToForeground()
{
#ifdef Q_OS_OSX
    d->foregroundCount = 0;
    bringApplicationToForegroundInternal();
#endif
}

void RunControl::reportApplicationStart()
{
    // QTC_CHECK(false); FIXME: Legacy, ToolRunner should emit started() instead.
    d->onToolStarted();
    emit started();
}

void RunControl::reportApplicationStop()
{
    // QTC_CHECK(false); FIXME: Legacy, ToolRunner should emit stopped() instead.
    if (d->state == RunControlPrivate::State::Stopped) {
        // FIXME: Currently various tool implementations call reportApplicationStop()
        // multiple times. Fix it there and then add a soft assert here.
        return;
    }
    d->onTargetStopped();
}

void RunControl::bringApplicationToForegroundInternal()
{
#ifdef Q_OS_OSX
    ProcessSerialNumber psn;
    GetProcessForPID(d->applicationProcessHandle.pid(), &psn);
    if (SetFrontProcess(&psn) == procNotFound && d->foregroundCount < 15) {
        // somehow the mac/carbon api says
        // "-600 no eligible process with specified process id"
        // if we call SetFrontProcess too early
        ++d->foregroundCount;
        QTimer::singleShot(200, this, &RunControl::bringApplicationToForegroundInternal);
    }
#endif
}

void RunControl::appendMessage(const QString &msg, Utils::OutputFormat format)
{
    emit appendMessageRequested(this, msg, format);
}

bool Runnable::canReUseOutputPane(const Runnable &other) const
{
    return d ? d->canReUseOutputPane(other.d) : (other.d.get() == 0);
}


// FIXME: Remove once ApplicationLauncher signalling does not depend on device.
static bool isSynchronousLauncher(RunControl *runControl)
{
    RunConfiguration *runConfig = runControl->runConfiguration();
    Target *target = runConfig ? runConfig->target() : nullptr;
    Kit *kit = target ? target->kit() : nullptr;
    Core::Id deviceId = DeviceTypeKitInformation::deviceTypeId(kit);
    return !deviceId.isValid() || deviceId == ProjectExplorer::Constants::DESKTOP_DEVICE_TYPE;
}


// SimpleTargetRunner

SimpleTargetRunner::SimpleTargetRunner(RunControl *runControl)
    : TargetRunner(runControl)
{
}

void SimpleTargetRunner::start()
{
    m_launcher.disconnect(this);

    Runnable r = runControl()->runnable();

    if (isSynchronousLauncher(runControl())) {

        connect(&m_launcher, &ApplicationLauncher::appendMessage,
                this, &TargetRunner::appendMessage);
        connect(&m_launcher, &ApplicationLauncher::processStarted,
                this, &SimpleTargetRunner::onProcessStarted);
        connect(&m_launcher, &ApplicationLauncher::processExited,
                this, &SimpleTargetRunner::onProcessFinished);

        QTC_ASSERT(r.is<StandardRunnable>(), return);
        const QString executable = r.as<StandardRunnable>().executable;
        if (executable.isEmpty()) {
            reportFailure(RunControl::tr("No executable specified."));
        }  else if (!QFileInfo::exists(executable)) {
            reportFailure(RunControl::tr("Executable %1 does not exist.")
                              .arg(QDir::toNativeSeparators(executable)));
        } else {
            QString msg = RunControl::tr("Starting %1...").arg(QDir::toNativeSeparators(executable)) + '\n';
            appendMessage(msg, Utils::NormalMessageFormat);
            m_launcher.start(r);
        }

    } else {

        connect(&m_launcher, &ApplicationLauncher::reportError,
                this, &TargetRunner::reportFailure);

        connect(&m_launcher, &ApplicationLauncher::remoteStderr,
                this, [this](const QByteArray &output) {
                    appendMessage(QString::fromUtf8(output), Utils::StdErrFormatSameLine);
                });

        connect(&m_launcher, &ApplicationLauncher::remoteStdout,
                this, [this](const QByteArray &output) {
                    appendMessage(QString::fromUtf8(output), Utils::StdOutFormatSameLine);
                });

        connect(&m_launcher, &ApplicationLauncher::finished,
                this, [this] {
                    m_launcher.disconnect(this);
                    reportSuccess();
                });

        connect(&m_launcher, &ApplicationLauncher::reportProgress,
                this, [this](const QString &progressString) {
                    appendMessage(progressString + '\n', Utils::NormalMessageFormat);
                });

        m_launcher.start(r, runControl()->device());
    }
}

void SimpleTargetRunner::stop()
{
    m_launcher.stop();
}

void SimpleTargetRunner::onProcessStarted()
{
    // Console processes only know their pid after being started
    runControl()->setApplicationProcessHandle(m_launcher.applicationPID());
    runControl()->bringApplicationToForeground();
    reportSuccess();
}

void SimpleTargetRunner::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    QString msg;
    QString exe = runControl()->runnable().as<StandardRunnable>().executable;
    if (status == QProcess::CrashExit)
        msg = tr("%1 crashed.").arg(QDir::toNativeSeparators(exe));
    else
        msg = tr("%1 exited with code %2").arg(QDir::toNativeSeparators(exe)).arg(exitCode);
    appendMessage(msg + '\n', Utils::NormalMessageFormat);
    reportStopped();
}


// TargetRunner

TargetRunner::TargetRunner(RunControl *runControl)
    : m_runControl(runControl)
{
    runControl->setTargetRunner(this);
}

TargetRunner::~TargetRunner()
{
}

RunControl *TargetRunner::runControl() const
{
    return m_runControl;
}

void TargetRunner::appendMessage(const QString &msg, OutputFormat format)
{
    m_runControl->appendMessage(msg, format);
}

IDevice::ConstPtr TargetRunner::device() const
{
    return m_runControl->device();
}

void TargetRunner::prepare()
{
    reportSuccess(); // By default nothing to do, all is fine.
}

void TargetRunner::start()
{
    reportSuccess();
}

void TargetRunner::stop()
{
    reportSuccess(); // By default all is fine.
}

void TargetRunner::reportStopped()
{
    m_runControl->d->onTargetStopped();
}

void TargetRunner::reportSuccess()
{
    m_runControl->d->onTargetSuccess();
}

void TargetRunner::reportFailure(const QString &msg)
{
    m_runControl->d->onTargetFailed(msg);
}

// ToolRunner

ToolRunner::ToolRunner(RunControl *runControl)
    : m_runControl(runControl)
{
    if (runControl)
        runControl->setToolRunner(this);
}

ToolRunner::~ToolRunner()
{
}

RunControl *ToolRunner::runControl() const
{
    return m_runControl;
}

void ToolRunner::appendMessage(const QString &msg, OutputFormat format)
{
    m_runControl->appendMessage(msg, format);
}

IDevice::ConstPtr ToolRunner::device() const
{
    return m_runControl->device();
}

const Runnable &ToolRunner::runnable() const
{
    return m_runControl->runnable();
}

const Connection &ToolRunner::connection() const
{
    return m_runControl->connection();
}

void ToolRunner::prepare()
{
    reportSuccess();
}

void ToolRunner::start()
{
    reportSuccess();
}

void ToolRunner::stop()
{
    reportSuccess();
}

void ToolRunner::reportStopped()
{
    m_runControl->d->onToolStopped();
}

void ToolRunner::reportSuccess()
{
    m_runControl->d->onToolSuccess();
}

void ToolRunner::reportFailure(const QString &msg)
{
    m_runControl->d->onToolFailed(msg);
}

} // namespace ProjectExplorer
