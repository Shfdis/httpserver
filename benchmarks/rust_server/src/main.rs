use hyper::server::conn::http1;
use hyper::service::service_fn;
use hyper::{Method, Request, Response, StatusCode};
use hyper_util::rt::TokioIo;
use http_body_util::{Full, BodyExt};
use bytes::Bytes;
use std::convert::Infallible;
use std::net::SocketAddr;
use tokio::net::TcpListener;

async fn handle_request(
    req: Request<hyper::body::Incoming>,
) -> Result<Response<Full<Bytes>>, Infallible> {
    match (req.method(), req.uri().path()) {
        (&Method::GET, "/echo") => {
            let mut body = Bytes::new();
            // Get query parameters
            if let Some(query) = req.uri().query() {
                if query.starts_with("msg=") {
                    let msg = &query[4..];
                    let decoded = percent_encoding::percent_decode_str(msg)
                        .decode_utf8_lossy()
                        .to_string();
                    body = Bytes::from(decoded);
                }
            }
            let body_len = body.len();
            
            let mut response = Response::new(Full::new(body));
            *response.status_mut() = StatusCode::OK;
            
            // Set headers
            let headers = response.headers_mut();
            headers.insert("Content-Length", body_len.to_string().parse().unwrap());
            headers.insert("Connection", "close".parse().unwrap());
            
            Ok(response)
        }
        (&Method::POST, "/echo") => {
            // Read body
            let body_bytes = match req.into_body().collect().await {
                Ok(collected) => collected.to_bytes(),
                Err(_) => {
                    let mut response = Response::new(Full::new(Bytes::from("Bad Request")));
                    *response.status_mut() = StatusCode::BAD_REQUEST;
                    return Ok(response);
                }
            };
            
            let body_len = body_bytes.len();
            let mut response = Response::new(Full::new(body_bytes));
            *response.status_mut() = StatusCode::OK;
            
            // Set headers
            let headers = response.headers_mut();
            headers.insert("Content-Length", body_len.to_string().parse().unwrap());
            headers.insert("Connection", "close".parse().unwrap());
            
            Ok(response)
        }
        _ => {
            let mut response = Response::new(Full::new(Bytes::from("Not Found")));
            *response.status_mut() = StatusCode::NOT_FOUND;
            let headers = response.headers_mut();
            headers.insert("Content-Length", "9".parse().unwrap());
            headers.insert("Connection", "close".parse().unwrap());
            Ok(response)
        }
    }
}

#[tokio::main(flavor = "multi_thread", worker_threads = 22)]
async fn main() -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let addr: SocketAddr = "127.0.0.1:8081".parse()?;
    let listener = TcpListener::bind(&addr).await?;
    println!("Tokio server listening on http://{}", addr);
    
    loop {
        // Accept connections asynchronously - this yields to the runtime
        // when no connection is available, allowing other tasks to run
        let (stream, _) = listener.accept().await?;
        let io = TokioIo::new(stream);
        
        // Spawn each connection handler immediately without waiting
        tokio::task::spawn(async move {
            if let Err(err) = http1::Builder::new()
                .serve_connection(io, service_fn(handle_request))
                .await
            {
                eprintln!("Error serving connection: {:?}", err);
            }
        });
    }
}
