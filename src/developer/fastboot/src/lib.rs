// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{bail, Result},
    command::Command,
    futures::{
        io::{AsyncRead, AsyncWrite},
        AsyncReadExt, AsyncWriteExt,
    },
    reply::Reply,
    std::convert::{TryFrom, TryInto},
};

pub mod command;
pub mod reply;

const MAX_PACKET_SIZE: usize = 64;
const READ_RETRY_MAX: usize = 100;

pub trait UploadProgressListener {
    fn on_started(&self, size: usize) -> Result<()>;
    fn on_progress(&self, bytes_written: u64) -> Result<()>;
    fn on_error(&self, error: &str) -> Result<()>;
    fn on_finished(&self) -> Result<()>;
}

async fn read_from_interface<T: AsyncRead + Unpin>(interface: &mut T) -> Result<Reply> {
    let mut buf: [u8; MAX_PACKET_SIZE] = [0; MAX_PACKET_SIZE];
    let size = interface.read(&mut buf).await?;
    let (trimmed, _) = buf.split_at(size);
    Reply::try_from(trimmed.to_vec())
}

async fn read_and_log_info<T: AsyncRead + Unpin>(interface: &mut T) -> Result<Reply> {
    let mut retry = 0;
    loop {
        match read_from_interface(interface).await {
            Ok(reply) => match reply {
                Reply::Info(msg) => log::info!("{}", msg),
                _ => return Ok(reply),
            },
            Err(e) => {
                log::warn!("error reading fastboot reply from usb interface: {:?}", e);
                retry += 1;
                if retry >= READ_RETRY_MAX {
                    log::error!("could not read reply: {:?}", e);
                    return Err(e);
                }
            }
        }
    }
}

pub async fn send<T: AsyncRead + AsyncWrite + Unpin>(
    cmd: Command,
    interface: &mut T,
) -> Result<Reply> {
    interface.write(&Vec::<u8>::try_from(cmd)?).await?;
    read_and_log_info(interface).await
}

pub async fn upload<T: AsyncRead + AsyncWrite + Unpin>(
    data: &[u8],
    interface: &mut T,
    listener: &impl UploadProgressListener,
) -> Result<Reply> {
    let reply = send(Command::Download(u32::try_from(data.len())?), interface).await?;
    match reply {
        Reply::Data(s) => {
            if s != u32::try_from(data.len())? {
                let err = format!(
                    "Target responded with wrong data size - received:{} expected:{}",
                    s,
                    data.len()
                );
                log::error!("{}", err);
                listener.on_error(&err)?;
                bail!(err);
            }
            listener.on_started(data.len())?;
            let mut t: usize = 0;
            // Chunk into 1mb chunks so that progress can be reported during
            // large writes.
            for chunk in data.chunks(1<<20) {
                match interface.write(&chunk).await {
                    Ok(x) => {
                        t += x;
                        listener
                            .on_progress(t.try_into().expect("usize should fit in u64"))?;
                    }
                    Err(e) => {
                        let err = format!("Could not write to usb interface: {:?}", e);
                        log::error!("{}", err);
                        listener.on_error(&err)?;
                        bail!(err);
                    }
                }
            }
            match read_and_log_info(interface).await {
                Ok(reply) => {
                    listener.on_finished()?;
                    Ok(reply)
                }
                Err(e) => {
                    let err = format!("Could not verify upload: {:?}", e);
                    log::error!("{}", err);
                    listener.on_error(&err)?;
                    bail!(err);
                }
            }
        }
        _ => bail!("Did not get expected Data reply: {:?}", reply),
    }
}

////////////////////////////////////////////////////////////////////////////////
// tests

#[cfg(test)]
mod test {
    use super::*;
    use command::ClientVariable;
    use futures::io::task::{Context, Poll};
    use std::pin::Pin;

    struct TestTransport {
        replies: Vec<Reply>,
    }

    impl AsyncRead for TestTransport {
        fn poll_read(
            self: Pin<&mut Self>,
            cx: &mut Context<'_>,
            buf: &mut [u8],
        ) -> Poll<std::io::Result<usize>> {
            match self.replies.pop() {
                Some(r) => {
                    let reply = Vec::<u8>::from(r);
                    buf[..reply.len()].copy_from_slice(&reply);
                    Ok(reply.len())
                }
                None => Ok(0),
            }
        }
    }

    impl AsyncWrite for TestTransport {
        fn poll_write(
            self: Pin<&mut Self>,
            cx: &mut Context<'_>,
            buf: &[u8],
        ) -> Poll<std::io::Result<usize>> {
            Ok(buf.len())
        }

        fn poll_flush(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
            unimplemented!();
        }

        fn poll_close(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<std::io::Result<()>> {
            unimplemented!();
        }
    }

    impl TestTransport {
        pub fn new() -> Self {
            TestTransport { replies: Vec::new() }
        }

        pub fn push(&mut self, reply: Reply) {
            self.replies.push(reply);
        }
    }

    #[test]
    fn test_send_does_not_return_info_replies() {
        let mut test_transport = TestTransport::new();
        test_transport.push(Reply::Okay("0.4".to_string()));
        let response = send(Command::GetVar(ClientVariable::Version), &mut test_transport);
        assert!(!response.is_err());
        assert_eq!(response.unwrap(), Reply::Okay("0.4".to_string()));

        test_transport.push(Reply::Okay("0.4".to_string()));
        test_transport.push(Reply::Info("Test".to_string()));
        let response_with_info =
            send(Command::GetVar(ClientVariable::Version), &mut test_transport);
        assert!(!response_with_info.is_err());
        assert_eq!(response_with_info.unwrap(), Reply::Okay("0.4".to_string()));

        test_transport.push(Reply::Okay("0.4".to_string()));
        for i in 0..10 {
            test_transport.push(Reply::Info(format!("Test {}", i).to_string()));
        }
        let response_with_info =
            send(Command::GetVar(ClientVariable::Version), &mut test_transport);
        assert!(!response_with_info.is_err());
        assert_eq!(response_with_info.unwrap(), Reply::Okay("0.4".to_string()));
    }

    #[test]
    fn test_uploading_data_to_partition() {
        let data: [u8; 1024] = [0; 1024];
        let mut test_transport = TestTransport::new();
        test_transport.push(Reply::Okay("Done Writing".to_string()));
        test_transport.push(Reply::Info("Writing".to_string()));
        test_transport.push(Reply::Data(1024));

        let response = upload(&data, &mut test_transport);
        assert!(!response.is_err());
        assert_eq!(response.unwrap(), Reply::Okay("Done Writing".to_string()));
    }

    #[test]
    fn test_uploading_data_with_unexpected_reply() {
        let data: [u8; 1024] = [0; 1024];
        let mut test_transport = TestTransport::new();
        test_transport.push(Reply::Info("Writing".to_string()));

        let response = upload(&data, &mut test_transport);
        assert!(response.is_err());
    }

    #[test]
    fn test_uploading_data_with_unexpected_data_size_reply() {
        let data: [u8; 1024] = [0; 1024];
        let mut test_transport = TestTransport::new();
        test_transport.push(Reply::Data(1000));

        let response = upload(&data, &mut test_transport);
        assert!(response.is_err());
    }
}
