// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context as _, Error},
    fidl::{endpoints::ClientEnd, prelude::*},
    fidl_fuchsia_developer_remotecontrol as rcs,
    fidl_fuchsia_overnet::{ServiceProviderRequest, ServiceProviderRequestStream},
    fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    futures::join,
    futures::prelude::*,
    hoist::{hoist, OvernetInstance},
    remote_control::RemoteControlService,
    std::rc::Rc,
    tracing::{error, info},
};

mod args;

async fn exec_server() -> Result<(), Error> {
    diagnostics_log::init!(&["remote-control"]);

    let service = Rc::new(RemoteControlService::new().await);

    let sc = service.clone();
    let onet_fut = async move {
        loop {
            let sc = sc.clone();
            let stream = (|| -> Result<_, Error> {
                let (s, p) =
                    fidl::Channel::create().context("creating ServiceProvider zx channel")?;
                let chan = fidl::AsyncChannel::from_channel(s)
                    .context("creating ServiceProvider async channel")?;
                let stream = ServiceProviderRequestStream::from_channel(chan);
                hoist()
                    .publish_service(rcs::RemoteControlMarker::PROTOCOL_NAME, ClientEnd::new(p))?;
                Ok(stream)
            })();

            let stream = match stream {
                Ok(stream) => stream,
                Err(err) => {
                    error!("Could not connect to overnet: {:?}", err);
                    break;
                }
            };

            let fut = stream.for_each_concurrent(None, move |svc| {
                let ServiceProviderRequest::ConnectToService {
                    chan,
                    info: _,
                    control_handle: _control_handle,
                } = svc.unwrap();
                let chan = fidl::AsyncChannel::from_channel(chan)
                    .context("failed to make async channel")
                    .unwrap();

                sc.clone().serve_stream(rcs::RemoteControlRequestStream::from_channel(chan))
            });
            info!("published remote control service to overnet");
            let res = fut.await;
            info!("connection to overnet lost: {:?}", res);
        }
    };

    let sc1 = service.clone();
    let mut fs = ServiceFs::new_local();
    fs.dir("svc").add_fidl_service(move |req| {
        fasync::Task::local(sc1.clone().serve_stream(req)).detach();
    });

    fs.take_and_serve_directory_handle()?;
    let fidl_fut = fs.collect::<()>();

    join!(fidl_fut, onet_fut);
    Ok(())
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    let args::RemoteControl { cmd } = argh::from_env();

    let res = match cmd {
        args::Command::DiagnosticsBridge(_) => diagnostics_bridge::exec_server().await,
        args::Command::RemoteControl(_) => exec_server().await,
    };

    if let Err(err) = res {
        error!(%err, "Error running command");
        std::process::exit(1);
    }
    Ok(())
}
