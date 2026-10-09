"""
registro.py - banco de dados das aparições (quem passou pela câmera e por quanto tempo)

Cada "aparição" (sessão) é o tempo contínuo em que um rosto ficou na câmera:
começa quando o rosto aparece e termina quando ele some. Fica tudo num arquivo
SQLite em pc/dados/monitoramento.db (só neste PC, fora do git):

    aparicoes(id, pessoa, inicio, fim, duracao, ativo, camera, confianca, foto)
      pessoa     nome cadastrado, ou "" = desconhecido
      inicio/fim horário (segundos desde 1970); fim vai sendo atualizado enquanto
                 a pessoa está na câmera
      ativo      1 = a pessoa ainda está na câmera agora
      confianca  maior semelhança com o cadastro (0..1)
      foto       JPEG pequeno do rosto (para identificar desconhecidos)

Regras:
  - aparições com menos de MIN_S segundos não são gravadas (piscadas do detector);
  - se a mesma pessoa CADASTRADA some e volta em até JUNTAR_S segundos, é a
    mesma aparição (o detector às vezes perde o rosto por um instante).
"""

import csv
import io
import sqlite3
import threading
import time
from collections import Counter
from datetime import datetime, timedelta
from pathlib import Path

MIN_S = 1.0          # aparição mais curta que é gravada
JUNTAR_S = 5.0       # pessoa cadastrada que volta em até isso = mesma aparição
SALVAR_S = 2.0       # de quanto em quanto tempo atualiza a aparição em andamento
FOTO_PX = 96         # lado da foto guardada
NOME_MIN = 0.3       # fração mínima de votos para a aparição ficar com um nome


class Registro:
    def __init__(self, arquivo):
        arquivo = Path(arquivo)
        arquivo.parent.mkdir(parents=True, exist_ok=True)
        self.arquivo = arquivo
        self.lock = threading.Lock()
        self.db = sqlite3.connect(str(arquivo), check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        with self.lock, self.db:
            self.db.execute("PRAGMA journal_mode=WAL")
            # O arquivo -wal cresce e não encolhe sozinho: limita a ~1 MB depois de cada checkpoint
            self.db.execute("PRAGMA journal_size_limit=1048576")
            self.db.execute("""
                CREATE TABLE IF NOT EXISTS aparicoes (
                    id        INTEGER PRIMARY KEY AUTOINCREMENT,
                    pessoa    TEXT    NOT NULL DEFAULT '',
                    inicio    REAL    NOT NULL,
                    fim       REAL    NOT NULL,
                    duracao   REAL    NOT NULL DEFAULT 0,
                    ativo     INTEGER NOT NULL DEFAULT 1,
                    camera    TEXT    NOT NULL DEFAULT '',
                    confianca REAL    NOT NULL DEFAULT 0,
                    foto      BLOB
                )""")
            self.db.execute("CREATE INDEX IF NOT EXISTS idx_inicio ON aparicoes(inicio)")
            self.db.execute("CREATE INDEX IF NOT EXISTS idx_pessoa ON aparicoes(pessoa)")
            # O programa foi fechado com gente na câmera: fecha essas aparições
            # no último horário salvo
            n = self.db.execute("UPDATE aparicoes SET ativo = 0 WHERE ativo = 1").rowcount
        if n:
            print(f"Registro: {n} aparição(ões) da última execução encerrada(s).")

    # ------------------------------------------------------------------
    # Gravação (chamado pela thread da câmera)
    # ------------------------------------------------------------------
    @staticmethod
    def _nome(sess):
        """Nome da aparição: o mais votado, se tiver pelo menos NOME_MIN dos votos."""
        votos = sess["votos"]
        total = sum(votos.values())
        nomes = [(n, c) for n, c in votos.most_common() if n]
        if nomes and total and nomes[0][1] / total >= NOME_MIN:
            return nomes[0][0]
        return ""

    def votar(self, tr, nome, score):
        """Um reconhecimento do rosto tr (nome '' = desconhecido)."""
        sess = self._sess(tr)
        sess["votos"][nome] += 1
        if nome:
            sess["conf"][nome] = max(sess["conf"].get(nome, 0.0), float(score))

    def _sess(self, tr):
        if "sess" not in tr:
            tr["sess"] = {"id": None, "votos": Counter(), "conf": {}, "salvo": 0.0}
        return tr["sess"]

    def atualizar(self, tracks, camera, frame=None, agora=None):
        """Chamado a cada quadro com os rostos visíveis: grava/atualiza as aparições."""
        agora = agora or time.time()
        for tr in tracks:
            sess = self._sess(tr)
            dur = tr["last"] - tr["first"]
            if sess["id"] is None:
                if dur >= MIN_S:
                    self._abrir(tr, sess, camera, frame)
            elif agora - sess["salvo"] >= SALVAR_S:
                self._salvar(tr, sess, ativo=1)

    def encerrar(self, tracks):
        """Os rostos tr sumiram da câmera: fecha as aparições deles."""
        for tr in tracks:
            sess = tr.get("sess")
            if sess and sess["id"] is not None:
                self._salvar(tr, sess, ativo=0)
                sess["id"] = None

    def _abrir(self, tr, sess, camera, frame):
        nome = self._nome(sess)
        foto = self._foto(frame, tr["box"]) if frame is not None else None
        with self.lock, self.db:
            if nome:   # a mesma pessoa saiu e voltou rapidinho: continua a aparição
                row = self.db.execute(
                    "SELECT id FROM aparicoes WHERE pessoa = ? AND ativo = 0 AND fim >= ? "
                    "ORDER BY fim DESC LIMIT 1", (nome, tr["first"] - JUNTAR_S)).fetchone()
                if row:
                    sess["id"] = row["id"]
                    self.db.execute("UPDATE aparicoes SET ativo = 1, fim = ? WHERE id = ?",
                                    (tr["last"], row["id"]))
                    sess["inicio_db"] = self.db.execute(
                        "SELECT inicio FROM aparicoes WHERE id = ?", (row["id"],)).fetchone()[0]
                    sess["salvo"] = time.time()
                    return
            cur = self.db.execute(
                "INSERT INTO aparicoes (pessoa, inicio, fim, duracao, ativo, camera, confianca, foto) "
                "VALUES (?, ?, ?, ?, 1, ?, ?, ?)",
                (nome, tr["first"], tr["last"], tr["last"] - tr["first"], camera,
                 sess["conf"].get(nome, 0.0), foto))
            sess["id"] = cur.lastrowid
            sess["inicio_db"] = tr["first"]
            sess["salvo"] = time.time()

    def _salvar(self, tr, sess, ativo):
        nome = self._nome(sess)
        inicio = sess.get("inicio_db", tr["first"])
        with self.lock, self.db:
            self.db.execute(
                "UPDATE aparicoes SET pessoa = ?, fim = ?, duracao = ?, ativo = ?, "
                "confianca = MAX(confianca, ?) WHERE id = ?",
                (nome, tr["last"], tr["last"] - inicio, ativo, sess["conf"].get(nome, 0.0), sess["id"]))
        sess["salvo"] = time.time()

    def renomear(self, antigo, novo):
        """A pessoa mudou de nome no cadastro: as aparições dela acompanham."""
        if not antigo or antigo == novo:
            return
        with self.lock, self.db:
            self.db.execute("UPDATE aparicoes SET pessoa = ? WHERE pessoa = ?", (novo, antigo))

    @staticmethod
    def _foto(frame, box):
        import cv2
        h, w = frame.shape[:2]
        x, y, bw, bh = box
        m = 0.25 * max(bw, bh)  # um pouco de margem em volta do rosto
        x0, y0 = int(max(0, x - m)), int(max(0, y - m))
        x1, y1 = int(min(w, x + bw + m)), int(min(h, y + bh + m))
        if x1 <= x0 or y1 <= y0:
            return None
        crop = cv2.resize(frame[y0:y1, x0:x1], (FOTO_PX, FOTO_PX), interpolation=cv2.INTER_AREA)
        ok, jpg = cv2.imencode(".jpg", crop, [cv2.IMWRITE_JPEG_QUALITY, 80])
        return jpg.tobytes() if ok else None

    # ------------------------------------------------------------------
    # Consultas (chamadas pelo servidor HTTP)
    # ------------------------------------------------------------------
    @staticmethod
    def _linha(r):
        return {"id": r["id"], "pessoa": r["pessoa"], "inicio": r["inicio"], "fim": r["fim"],
                "duracao": round(r["duracao"], 1), "ativo": bool(r["ativo"]),
                "camera": r["camera"], "confianca": round(r["confianca"], 3),
                "foto": r["foto"] is not None}

    def recentes(self, n=5):
        with self.lock:
            rows = self.db.execute(
                "SELECT id, pessoa, inicio, fim, duracao, ativo, camera, confianca, foto IS NOT NULL AS foto "
                "FROM aparicoes ORDER BY inicio DESC LIMIT ?", (n,)).fetchall()
            total = self.db.execute("SELECT COUNT(*) FROM aparicoes").fetchone()[0]
        return {"itens": [self._linha(r) for r in rows], "total": total, "agora": time.time()}

    def foto(self, ident):
        with self.lock:
            r = self.db.execute("SELECT foto FROM aparicoes WHERE id = ?", (ident,)).fetchone()
        return r["foto"] if r else None

    def pessoas(self):
        with self.lock:
            return [r[0] for r in self.db.execute(
                "SELECT DISTINCT pessoa FROM aparicoes WHERE pessoa <> '' ORDER BY pessoa COLLATE NOCASE")]

    @staticmethod
    def _filtro(de, ate, pessoa):
        """Monta o WHERE. de/ate: 'AAAA-MM-DD' (dias inteiros, horário local)."""
        where, args = [], []
        if de:
            where.append("inicio >= ?")
            args.append(datetime.strptime(de, "%Y-%m-%d").timestamp())
        if ate:
            where.append("inicio < ?")
            args.append((datetime.strptime(ate, "%Y-%m-%d") + timedelta(days=1)).timestamp())
        if pessoa == "__conhecidos":
            where.append("pessoa <> ''")
        elif pessoa == "__desconhecidos":
            where.append("pessoa = ''")
        elif pessoa:
            where.append("pessoa = ?")
            args.append(pessoa)
        return (" WHERE " + " AND ".join(where)) if where else "", args

    def relatorio(self, de=None, ate=None, pessoa=None, pagina=1, por_pagina=100):
        w, a = self._filtro(de, ate, pessoa)
        with self.lock:
            tot = self.db.execute(
                f"SELECT COUNT(*) AS n, COALESCE(SUM(duracao),0) AS tempo, COALESCE(AVG(duracao),0) AS media, "
                f"COALESCE(MAX(duracao),0) AS maior, "
                f"COUNT(DISTINCT CASE WHEN pessoa <> '' THEN pessoa END) AS conhecidas, "
                f"SUM(CASE WHEN pessoa = '' THEN 1 ELSE 0 END) AS desconhecidas "
                f"FROM aparicoes{w}", a).fetchone()
            por_pessoa = self.db.execute(
                f"SELECT pessoa, COUNT(*) AS n, SUM(duracao) AS tempo, AVG(duracao) AS media, "
                f"MAX(duracao) AS maior, MIN(inicio) AS primeira, MAX(fim) AS ultima "
                f"FROM aparicoes{w} GROUP BY pessoa ORDER BY tempo DESC", a).fetchall()
            por_dia = self.db.execute(
                f"SELECT date(inicio, 'unixepoch', 'localtime') AS dia, COUNT(*) AS n, "
                f"SUM(duracao) AS tempo, COUNT(DISTINCT CASE WHEN pessoa <> '' THEN pessoa END) AS conhecidas, "
                f"SUM(CASE WHEN pessoa = '' THEN 1 ELSE 0 END) AS desconhecidas "
                f"FROM aparicoes{w} GROUP BY dia ORDER BY dia", a).fetchall()
            por_hora = self.db.execute(
                f"SELECT CAST(strftime('%H', inicio, 'unixepoch', 'localtime') AS INTEGER) AS hora, "
                f"COUNT(*) AS n FROM aparicoes{w} GROUP BY hora ORDER BY hora", a).fetchall()
            pagina = max(1, int(pagina))
            lista = self.db.execute(
                f"SELECT id, pessoa, inicio, fim, duracao, ativo, camera, confianca, foto IS NOT NULL AS foto "
                f"FROM aparicoes{w} ORDER BY inicio DESC LIMIT ? OFFSET ?",
                a + [por_pagina, (pagina - 1) * por_pagina]).fetchall()
        return {
            "filtro": {"de": de, "ate": ate, "pessoa": pessoa},
            "total": {"aparicoes": tot["n"], "tempo": round(tot["tempo"], 1), "media": round(tot["media"], 1),
                      "maior": round(tot["maior"], 1), "conhecidas": tot["conhecidas"],
                      "desconhecidas": tot["desconhecidas"] or 0},
            "porPessoa": [{"pessoa": r["pessoa"], "aparicoes": r["n"], "tempo": round(r["tempo"], 1),
                           "media": round(r["media"], 1), "maior": round(r["maior"], 1),
                           "primeira": r["primeira"], "ultima": r["ultima"]} for r in por_pessoa],
            "porDia": [{"dia": r["dia"], "aparicoes": r["n"], "tempo": round(r["tempo"], 1),
                        "conhecidas": r["conhecidas"], "desconhecidas": r["desconhecidas"]} for r in por_dia],
            "porHora": [{"hora": r["hora"], "aparicoes": r["n"]} for r in por_hora],
            "lista": [self._linha(r) for r in lista],
            "pagina": pagina,
            "porPagina": por_pagina,
            "pessoas": self.pessoas(),
            "geradoEm": time.time(),
        }

    def csv(self, de=None, ate=None, pessoa=None):
        """Todas as aparições do filtro em CSV (separador ';' e BOM: abre certo no Excel)."""
        w, a = self._filtro(de, ate, pessoa)
        with self.lock:
            rows = self.db.execute(
                f"SELECT id, pessoa, inicio, fim, duracao, ativo, camera, confianca "
                f"FROM aparicoes{w} ORDER BY inicio", a).fetchall()
        out = io.StringIO()
        wr = csv.writer(out, delimiter=";", lineterminator="\r\n")
        wr.writerow(["id", "pessoa", "data", "entrada", "saida", "duracao_s", "duracao",
                     "na_camera_agora", "camera", "confianca_%"])
        for r in rows:
            ini, fim = datetime.fromtimestamp(r["inicio"]), datetime.fromtimestamp(r["fim"])
            wr.writerow([r["id"], r["pessoa"] or "Desconhecido", ini.strftime("%d/%m/%Y"),
                         ini.strftime("%H:%M:%S"), fim.strftime("%H:%M:%S"),
                         f"{r['duracao']:.1f}".replace(".", ","), fmt_dur(r["duracao"]),
                         "sim" if r["ativo"] else "não", r["camera"],
                         f"{r['confianca'] * 100:.0f}" if r["pessoa"] else ""])
        return ("﻿" + out.getvalue()).encode("utf-8")


    # ------------------------------------------------------------------
    # Consumo e limpeza (página de relatórios)
    # ------------------------------------------------------------------
    def tamanho_arquivo(self):
        """Bytes do banco no disco (o .db mais os arquivos -wal e -shm do SQLite)."""
        return sum(p.stat().st_size for p in (self.arquivo, Path(f"{self.arquivo}-wal"),
                                               Path(f"{self.arquivo}-shm")) if p.exists())

    def consumo(self):
        with self.lock:
            r = self.db.execute(
                "SELECT COUNT(*) AS n, COUNT(foto) AS fotos, COALESCE(SUM(LENGTH(foto)), 0) AS bytes_fotos, "
                "MIN(inicio) AS primeira, MAX(inicio) AS ultima FROM aparicoes").fetchone()
        return {"arquivo": str(self.arquivo.name), "bytes": self.tamanho_arquivo(),
                "registros": r["n"], "fotos": r["fotos"], "bytesFotos": r["bytes_fotos"],
                "primeira": r["primeira"], "ultima": r["ultima"]}

    def limpar(self, antes, so_fotos=False, previa=False):
        """Apaga as aparições que começaram antes de `antes` (epoch). Quem está na
        câmera agora nunca é apagado. so_fotos: mantém os registros e tira só as fotos.
        previa: só conta. Depois de apagar, compacta o arquivo (VACUUM) para o espaço
        voltar para o disco."""
        where = "inicio < ? AND ativo = 0" + (" AND foto IS NOT NULL" if so_fotos else "")
        with self.lock:
            r = self.db.execute(f"SELECT COUNT(*) AS n, COALESCE(SUM(LENGTH(foto)), 0) AS b "
                                f"FROM aparicoes WHERE {where}", (antes,)).fetchone()
            res = {"registros": r["n"], "bytesFotos": r["b"]}
            if previa or not r["n"]:
                return res
            antes_bytes = self.tamanho_arquivo()
            with self.db:
                if so_fotos:
                    self.db.execute(f"UPDATE aparicoes SET foto = NULL WHERE {where}", (antes,))
                else:
                    self.db.execute(f"DELETE FROM aparicoes WHERE {where}", (antes,))
            self.db.execute("VACUUM")
            self.db.execute("PRAGMA wal_checkpoint(TRUNCATE)")
            res["liberado"] = max(0, antes_bytes - self.tamanho_arquivo())
        print(f"Registro: {res['registros']} aparição(ões) {'sem foto agora' if so_fotos else 'apagada(s)'}, "
              f"{res['liberado'] / 1024:.0f} KB liberados")
        return res


def fmt_dur(s):
    s = int(round(s))
    h, m, s = s // 3600, s % 3600 // 60, s % 60
    return f"{h}h{m:02d}m{s:02d}s" if h else f"{m}m{s:02d}s" if m else f"{s}s"
