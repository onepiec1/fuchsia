// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use std::{fs::File, path::PathBuf, rc::Rc, sync::Arc};

use anyhow::Result;
use argh::{CommandInfo, FromArgs, SubCommand, SubCommands};
use async_trait::async_trait;
use errors::{ffx_error, ResultExt};
use ffx_command::{DaemonVersionCheck, Ffx, FfxCommandLine, ToolRunner, ToolSuite};
use ffx_config::EnvironmentContext;
use ffx_core::Injector;
use fidl::endpoints::Proxy;
use fidl_fuchsia_developer_ffx as ffx_fidl;
use selectors::{self, VerboseError};
use std::time::Duration;

use crate::FhoToolMetadata;

#[derive(FromArgs)]
#[argh(subcommand)]
enum FhoHandler<M: FfxMain> {
    //FhoVersion1(M),
    /// Run the tool as if under ffx
    Standalone(M::Command),
    /// Print out the subtool's metadata json
    Metadata(MetadataCmd),
}

#[derive(FromArgs)]
#[argh(subcommand, name = "metadata", description = "Print out this subtool's FHO metadata json")]
struct MetadataCmd {
    #[argh(positional)]
    output_path: Option<PathBuf>,
}

#[derive(FromArgs)]
/// Fuchsia Host Objects Runner
struct ToolCommand<M: FfxMain> {
    #[argh(subcommand)]
    subcommand: FhoHandler<M>,
}

struct FhoSuite<M> {
    ffx: Ffx,
    context: EnvironmentContext,
    _p: std::marker::PhantomData<fn(M) -> ()>,
}

impl<M> Clone for FhoSuite<M> {
    fn clone(&self) -> Self {
        Self { ffx: self.ffx.clone(), context: self.context.clone(), _p: self._p.clone() }
    }
}

struct FhoTool<M: FfxMain> {
    suite: FhoSuite<M>,
    command: ToolCommand<M>,
}

pub struct FhoEnvironment<'a> {
    pub ffx: &'a Ffx,
    pub context: &'a EnvironmentContext,
    pub injector: &'a dyn Injector,
}

impl MetadataCmd {
    fn print(&self, info: &CommandInfo) -> Result<()> {
        let meta = FhoToolMetadata::new(info.name, info.description);
        match &self.output_path {
            Some(path) => serde_json::to_writer_pretty(&File::create(path)?, &meta)?,
            None => serde_json::to_writer_pretty(&std::io::stdout(), &meta)?,
        };
        Ok(())
    }
}

#[async_trait(?Send)]
impl<M: FfxMain> ToolRunner for FhoTool<M> {
    fn forces_stdout_log(&self) -> bool {
        M::forces_stdout_log()
    }

    async fn run(self: Box<Self>) -> Result<(), anyhow::Error> {
        match self.command.subcommand {
            FhoHandler::Metadata(metadata) => metadata.print(M::Command::COMMAND),
            FhoHandler::Standalone(tool) => {
                let cache_path = self.suite.context.get_cache_path()?;
                std::fs::create_dir_all(&cache_path)?;
                let hoist_cache_dir = tempfile::tempdir_in(&cache_path)?;
                let build_info = self.suite.context.build_info();
                let injector = self
                    .suite
                    .ffx
                    .initialize_overnet(
                        hoist_cache_dir.path(),
                        None,
                        DaemonVersionCheck::SameVersionInfo(build_info),
                    )
                    .await?;
                let env = FhoEnvironment {
                    ffx: &self.suite.ffx,
                    context: &self.suite.context,
                    injector: &injector,
                };
                let main = M::from_env(env, tool).await?;
                main.main().await
            }
        }
    }
}

impl<M: FfxMain> ToolSuite for FhoSuite<M> {
    fn from_env(ffx: &Ffx, context: &EnvironmentContext) -> Result<Self, anyhow::Error> {
        let ffx = ffx.clone();
        let context = context.clone();
        Ok(Self { ffx: ffx, context: context, _p: Default::default() })
    }

    fn global_command_list() -> &'static [&'static argh::CommandInfo] {
        FhoHandler::<M>::COMMANDS
    }

    fn try_from_args(
        &self,
        cmd: &FfxCommandLine,
        args: &[&str],
    ) -> Result<Option<Box<dyn ToolRunner>>, argh::EarlyExit> {
        let found = FhoTool {
            suite: self.clone(),
            command: ToolCommand::<M>::from_args(&Vec::from_iter(cmd.cmd_iter()), args)?,
        };
        Ok(Some(Box::new(found)))
    }

    fn redact_arg_values(
        &self,
        cmd: &FfxCommandLine,
        args: &[&str],
    ) -> Result<Vec<String>, argh::EarlyExit> {
        let cmd_vec = Vec::from_iter(cmd.cmd_iter());
        ToolCommand::<M>::redact_arg_values(&cmd_vec, args)
    }
}

#[async_trait(?Send)]
pub trait FfxTool: Sized + 'static {
    type Command: FromArgs + SubCommand + 'static;

    fn forces_stdout_log() -> bool;
    async fn from_env(env: FhoEnvironment<'_>, cmd: Self::Command) -> Result<Self>;
}

#[async_trait(?Send)]
pub trait FfxMain: FfxTool {
    /// The entrypoint of the tool. Once FHO has set up the environment for the tool, this is
    /// invoked. Should not be invoked directly unless for testing.
    async fn main(self) -> Result<()>;

    /// Executes the tool. This is intended to be invoked by the user in main.
    async fn execute_tool() {
        let result = ffx_command::run::<FhoSuite<Self>>().await;

        if let Err(err) = &result {
            let mut out = std::io::stderr();
            // abort hard on a failure to print the user error somehow
            errors::write_result(err, &mut out).unwrap();
            ffx_command::report_user_error(err).await.unwrap();
            ffx_config::print_log_hint(&mut out).await;
        }

        std::process::exit(result.exit_code());
    }
}

#[async_trait(?Send)]
pub trait TryFromEnv: Sized {
    async fn try_from_env(env: &FhoEnvironment<'_>) -> Result<Self>;
}

#[async_trait(?Send)]
pub trait CheckEnv {
    async fn check_env(self, env: &FhoEnvironment<'_>) -> Result<()>;
}

#[async_trait(?Send)]
impl<T> TryFromEnv for Arc<T>
where
    T: TryFromEnv,
{
    async fn try_from_env(env: &FhoEnvironment<'_>) -> Result<Self> {
        T::try_from_env(env).await.map(Arc::new)
    }
}

#[async_trait(?Send)]
impl<T> TryFromEnv for Rc<T>
where
    T: TryFromEnv,
{
    async fn try_from_env(env: &FhoEnvironment<'_>) -> Result<Self> {
        T::try_from_env(env).await.map(Rc::new)
    }
}

#[async_trait(?Send)]
impl<T> TryFromEnv for Box<T>
where
    T: TryFromEnv,
{
    async fn try_from_env(env: &FhoEnvironment<'_>) -> Result<Self> {
        T::try_from_env(env).await.map(Box::new)
    }
}

#[async_trait(?Send)]
impl<T> TryFromEnv for Option<T>
where
    T: TryFromEnv,
{
    async fn try_from_env(env: &FhoEnvironment<'_>) -> Result<Self> {
        Ok(T::try_from_env(env).await.ok())
    }
}

#[async_trait(?Send)]
impl<T> TryFromEnv for Result<T>
where
    T: TryFromEnv,
{
    async fn try_from_env(env: &FhoEnvironment<'_>) -> Result<Self> {
        Ok(T::try_from_env(env).await)
    }
}

/// Checks if the experimental config flag is set. This gates the execution of the command.
/// If the flag is set to `true`, this returns `Ok(())`, else returns an error.
pub struct AvailabilityFlag<T>(pub T);

#[async_trait(?Send)]
impl<T: AsRef<str>> CheckEnv for AvailabilityFlag<T> {
    async fn check_env(self, _env: &FhoEnvironment<'_>) -> Result<()> {
        let flag = self.0.as_ref();
        if ffx_config::get(flag).await.unwrap_or(false) {
            Ok(())
        } else {
            errors::ffx_bail!(
                "This is an experimental subcommand.  To enable this subcommand run 'ffx config set {} true'",
                flag
            )
        }
    }
}

/// A trait for looking up a Fuchsia component when using the Protocol struct.
///
/// Example usage;
/// ```rust
/// struct FooSelector;
/// impl FuchsiaComponentSelector for FooSelector {
///     const SELECTOR: &'static str = "core/selector/thing";
/// }
///
/// #[derive(FfxTool)]
/// struct Tool {
///     foo_proxy: Protocol<FooProxy, FooSelector>,
/// }
/// ```
pub trait FuchsiaComponentSelector {
    const SELECTOR: &'static str;
}

/// A wrapper type used to look up protocols on a Fuchsia target. Whatever has been set as the
/// default target in the environment will be where the proxy is connected.
#[derive(Debug, Clone)]
pub struct Protocol<P: Clone, S> {
    proxy: P,
    _s: std::marker::PhantomData<fn(S) -> ()>,
}

impl<P: Clone, S> Protocol<P, S> {
    pub fn new(proxy: P) -> Self {
        Self { proxy, _s: Default::default() }
    }
}

impl<P: Clone, S> std::ops::Deref for Protocol<P, S> {
    type Target = P;

    fn deref(&self) -> &Self::Target {
        &self.proxy
    }
}

#[async_trait(?Send)]
impl<P: Proxy + Clone, S: FuchsiaComponentSelector> TryFromEnv for Protocol<P, S>
where
    P::Protocol: fidl::endpoints::DiscoverableProtocolMarker,
{
    async fn try_from_env(env: &FhoEnvironment<'_>) -> Result<Self> {
        let (proxy, server_end) = fidl::endpoints::create_proxy::<P::Protocol>()?;
        let _ = selectors::parse_selector::<VerboseError>(S::SELECTOR)?;
        let retry_count = 1;
        let mut tries = 0;
        // TODO(fxbug.dev/113143): Remove explicit retries/timeouts here so they can be
        // configurable instead.
        let rcs_instance = loop {
            tries += 1;
            let res = env.injector.remote_factory().await;
            if res.is_ok() || tries > retry_count {
                break res;
            }
        }?;
        rcs::connect_with_timeout(
            Duration::from_secs(15),
            S::SELECTOR,
            &rcs_instance,
            server_end.into_channel(),
        )
        .await?;
        Ok(Protocol::new(proxy))
    }
}

#[derive(Debug, Clone)]
pub struct DaemonProtocol<P: Clone> {
    proxy: P,
}

impl<P: Clone> DaemonProtocol<P> {
    pub fn new(proxy: P) -> Self {
        Self { proxy }
    }
}

impl<P: Clone> DaemonProtocol<P> {
    pub fn into_inner(self) -> P {
        self.proxy
    }
}

impl<P: Clone> std::ops::Deref for DaemonProtocol<P> {
    type Target = P;

    fn deref(&self) -> &Self::Target {
        &self.proxy
    }
}

#[async_trait(?Send)]
impl<P: Proxy + Clone> TryFromEnv for DaemonProtocol<P>
where
    P::Protocol: fidl::endpoints::DiscoverableProtocolMarker,
{
    async fn try_from_env(env: &FhoEnvironment<'_>) -> Result<Self> {
        let (proxy, server_end) = fidl::endpoints::create_proxy::<P::Protocol>()?;
        let daemon = env.injector.daemon_factory().await?;
        let svc_name = <P::Protocol as fidl::endpoints::DiscoverableProtocolMarker>::PROTOCOL_NAME;

        daemon.connect_to_protocol(svc_name, server_end.into_channel()).await?.map_err(
            |e| -> anyhow::Error {
                match e {
                    ffx_fidl::DaemonError::ProtocolNotFound => ffx_error!(format!(
                        "The daemon protocol '{svc_name}' did not match any protocols on the daemon
If you are not developing this plugin or the protocol it connects to, then this is a bug

Please report it at http://fxbug.dev/new/ffx+User+Bug."
                    ))
                    .into(),
                    ffx_fidl::DaemonError::ProtocolOpenError => ffx_error!(format!(
                        "The daemon protocol '{svc_name}' failed to open on the daemon.

If you are developing the protocol, there may be an internal failure when invoking the start
function. See the ffx.daemon.log for details at `ffx config get log.dir -p sub`.

If you are NOT developing this plugin or the protocol it connects to, then this is a bug.

Please report it at http://fxbug.dev/new/ffx+User+Bug."
                    ))
                    .into(),
                    unexpected => ffx_error!(format!(
"While attempting to open the daemon protocol '{svc_name}', received an unexpected error:

{unexpected:?}

This is not intended behavior and is a bug.
Please report it at http://fxbug.dev/new/ffx+User+Bug."

                                    ))
                    .into(),
                }
            },
        )?;
        Ok(DaemonProtocol { proxy })
    }
}

#[async_trait(?Send)]
impl TryFromEnv for ffx_fidl::DaemonProxy {
    async fn try_from_env(env: &FhoEnvironment<'_>) -> Result<Self> {
        env.injector.daemon_factory().await
    }
}

#[async_trait(?Send)]
impl TryFromEnv for ffx_fidl::TargetProxy {
    async fn try_from_env(env: &FhoEnvironment<'_>) -> Result<Self> {
        env.injector.target_factory().await
    }
}

#[async_trait(?Send)]
impl TryFromEnv for ffx_fidl::FastbootProxy {
    async fn try_from_env(env: &FhoEnvironment<'_>) -> Result<Self> {
        env.injector.fastboot_factory().await
    }
}

#[async_trait(?Send)]
impl TryFromEnv for ffx_writer::Writer {
    async fn try_from_env(env: &FhoEnvironment<'_>) -> Result<Self> {
        env.injector.writer().await
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    // This keeps the macros from having compiler errors.
    use crate as fho;
    use crate::testing::FakeInjector;
    use crate::{testing, FhoVersion};
    use argh::FromArgs;
    use async_trait::async_trait;
    use fho_macro::FfxTool;
    use std::cell::RefCell;

    struct NewTypeString(String);

    #[async_trait(?Send)]
    impl TryFromEnv for NewTypeString {
        async fn try_from_env(_env: &FhoEnvironment<'_>) -> Result<Self> {
            Ok(Self(String::from("foobar")))
        }
    }

    #[derive(Debug, FromArgs)]
    #[argh(subcommand, name = "fake", description = "fake command")]
    struct FakeCommand {
        #[argh(positional)]
        /// just needs a doc here so the macro doesn't complain.
        stuff: String,
    }

    thread_local! {
        static SIMPLE_CHECK_COUNTER: RefCell<u64> = RefCell::new(0);
    }

    struct SimpleCheck(bool);

    #[async_trait(?Send)]
    impl CheckEnv for SimpleCheck {
        async fn check_env(self, _env: &FhoEnvironment<'_>) -> Result<()> {
            SIMPLE_CHECK_COUNTER.with(|counter| *counter.borrow_mut() += 1);
            if self.0 {
                Ok(())
            } else {
                Err(anyhow::anyhow!("SimpleCheck was false"))
            }
        }
    }

    #[derive(FfxTool)]
    #[ffx(forces_stdout_logs)]
    #[check(SimpleCheck(true))]
    struct FakeTool {
        from_env_string: NewTypeString,
        #[command]
        fake_command: FakeCommand,
        writer: ffx_writer::Writer,
    }

    #[async_trait(?Send)]
    impl FfxMain for FakeTool {
        async fn main(self) -> Result<()> {
            assert_eq!(self.from_env_string.0, "foobar");
            assert_eq!(self.fake_command.stuff, "stuff");
            self.writer.line("junk-line").unwrap();
            Ok(())
        }
    }

    fn setup_fho_items<T: FfxMain>() -> (Ffx, EnvironmentContext, FakeInjector, ToolCommand<T>) {
        let context = ffx_config::EnvironmentContext::default();
        let injector = testing::FakeInjectorBuilder::new()
            .writer_closure(|| async { Ok(ffx_writer::Writer::new(None)) })
            .build();
        // Runs the command line tool as if under ffx (first version of fho invocation).
        let ffx_cmd_line = ffx_command::FfxCommandLine::new(
            None,
            vec!["ffx".to_owned(), "fake".to_owned(), "stuff".to_owned()],
        )
        .unwrap();
        let ffx = ffx_cmd_line.parse::<FhoSuite<T>>();

        let tool_cmd = ToolCommand::<T>::from_args(
            &Vec::from_iter(ffx_cmd_line.cmd_iter()),
            &Vec::from_iter(ffx_cmd_line.args_iter()),
        )
        .unwrap();
        (ffx, context, injector, tool_cmd)
    }

    // The main testing part will happen in the `main()` function of the tool.
    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_run_fake_tool() {
        let (ffx, context, injector, tool_cmd) = setup_fho_items::<FakeTool>();
        let fho_env = FhoEnvironment { ffx: &ffx, context: &context, injector: &injector };

        assert_eq!(
            SIMPLE_CHECK_COUNTER.with(|counter| *counter.borrow()),
            0,
            "tool pre-check should not have been called yet"
        );
        let fake_tool = match tool_cmd.subcommand {
            FhoHandler::Standalone(t) => FakeTool::from_env(fho_env, t).await.unwrap(),
            FhoHandler::Metadata(_) => panic!("Not testing metadata generation"),
        };
        assert_eq!(
            SIMPLE_CHECK_COUNTER.with(|counter| *counter.borrow()),
            1,
            "tool pre-check should have been called once"
        );
        fake_tool.main().await.unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn negative_precheck_fails() {
        #[derive(Debug, FfxTool)]
        #[check(SimpleCheck(false))]
        struct FakeToolWillFail {
            #[command]
            _fake_command: FakeCommand,
        }
        #[async_trait(?Send)]
        impl FfxMain for FakeToolWillFail {
            async fn main(self) -> Result<()> {
                panic!("This should never get called")
            }
        }

        let (ffx, context, injector, tool_cmd) = setup_fho_items::<FakeToolWillFail>();
        let fho_env = FhoEnvironment { ffx: &ffx, context: &context, injector: &injector };

        assert_eq!(
            SIMPLE_CHECK_COUNTER.with(|counter| *counter.borrow()),
            0,
            "tool pre-check should not have been called yet"
        );
        match tool_cmd.subcommand {
            FhoHandler::Standalone(t) => FakeToolWillFail::from_env(fho_env, t)
                .await
                .expect_err("Should not have been able to create tool with a negative pre-check"),
            FhoHandler::Metadata(_) => panic!("Not testing metadata generation"),
        };
        assert_eq!(
            SIMPLE_CHECK_COUNTER.with(|counter| *counter.borrow()),
            1,
            "tool pre-check should have been called once"
        );
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn present_metadata() {
        let tmpdir = tempfile::tempdir().expect("tempdir");

        let ffx = Ffx::from_args(&["ffx"], &[]).expect("ffx command line to parse");
        let context = EnvironmentContext::default();
        let suite: FhoSuite<FakeTool> = FhoSuite { ffx, context, _p: Default::default() };
        let output_path = tmpdir.path().join("metadata.json");
        let subcommand =
            FhoHandler::Metadata(MetadataCmd { output_path: Some(output_path.clone()) });
        let command = ToolCommand { subcommand };
        let tool = Box::new(FhoTool { suite, command });

        tool.run().await.expect("running metadata command");

        let read_metadata: FhoToolMetadata =
            serde_json::from_reader(File::open(output_path).expect("opening metadata"))
                .expect("parsing metadata");
        assert_eq!(
            read_metadata,
            FhoToolMetadata {
                name: "fake".to_owned(),
                description: "fake command".to_owned(),
                requires_fho: 0,
                fho_details: FhoVersion::FhoVersion0 {},
            }
        );
    }
}
