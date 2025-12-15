import asyncio
import logging
from punto.daemon.service import PuntoService

def main() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s"
    )
    
    service = PuntoService()
    try:
        asyncio.run(service.run())
    except KeyboardInterrupt:
        logging.info("Interrupted by user.")

if __name__ == "__main__":
    main()
