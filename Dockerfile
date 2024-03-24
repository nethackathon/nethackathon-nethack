FROM ubuntu:22.04

ENV USERNAME nethackathon
ENV HOME /home/${USERNAME}
ENV HACKDIR /home/${USERNAME}/games

RUN apt-get update && \
    apt-get install -y sudo && \
    apt-get install -y build-essential && \
    apt-get install -y git && \
    apt-get install -y curl && \
    apt-get install -y libncurses5-dev && \
    apt-get install -y groff && \
    apt-get install -y gdb && \
    apt-get install -y bsdmainutils

RUN useradd -m ${USERNAME} && \
    usermod -aG sudo ${USERNAME} && \
    echo "${USERNAME} ALL=(ALL) NOPASSWD: ALL" >> /etc/sudoers

WORKDIR ${HOME}

USER ${USERNAME}

VOLUME ${HOME}

CMD ["/bin/bash"]
