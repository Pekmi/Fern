using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using System.Collections.Generic;
using Microsoft.ML.OnnxRuntimeGenAI;

namespace FernUI.Models
{
    public class AiAnalysisService : IDisposable
    {
        private static AiAnalysisService? _instance;
        private static readonly SemaphoreSlim _lock = new SemaphoreSlim(1, 1);

        private Model _model;
        private MultiModalProcessor? _processor;
        private Tokenizer? _tokenizer;

        private AiAnalysisService(string modelDir)
        {
            // Initialise le modèle en forçant l'utilisation du GPU via DirectML
            var config = new Config(modelDir);
            config.ClearProviders();
            config.AppendProvider("dml");
            _model = new Model(config);
            
            try
            {
                // Tente d'initialiser le processeur Multimodal (pour Phi-4 Vision/Audio)
                _processor = new MultiModalProcessor(_model);
            }
            catch
            {
                // Fallback si le processeur multimodal n'est pas disponible dans cette version
                _tokenizer = new Tokenizer(_model);
            }
        }
        public static async Task<AiAnalysisService> GetInstanceAsync(string modelDir)
        {
            string logPath = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "fern_ai_log.txt");
            if (_instance == null)
            {
                File.AppendAllText(logPath, $"[{DateTime.Now:HH:mm:ss}] Attente du verrou de GetInstanceAsync...\n");
                await _lock.WaitAsync();
                try
                {
                    if (_instance == null)
                    {
                        File.AppendAllText(logPath, $"[{DateTime.Now:HH:mm:ss}] Chargement du modèle en RAM/VRAM sur un thread séparé...\n");
                        // Le chargement de 4.5 Go en RAM/VRAM est lourd, on le fait sur un thread de fond
                        await Task.Run(() => {
                            _instance = new AiAnalysisService(modelDir);
                        });
                        File.AppendAllText(logPath, $"[{DateTime.Now:HH:mm:ss}] Modèle chargé avec succès.\n");
                    }
                }
                catch(Exception ex)
                {
                    File.AppendAllText(logPath, $"[{DateTime.Now:HH:mm:ss}] EXCEPTION GetInstanceAsync: {ex}\n");
                    throw;
                }
                finally
                {
                    _lock.Release();
                }
            }
            return _instance;
        }

        /// <summary>
        /// Analyse une série d'images (ou une seule) avec un prompt donné.
        /// </summary>
        public async Task<string> AnalyzeFramesAsync(string[] imagePaths, string prompt)
        {
            string logPath = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "fern_ai_log.txt");
            File.AppendAllText(logPath, $"[{DateTime.Now:HH:mm:ss}] Attente du verrou AnalyzeFramesAsync...\n");
            // On s'assure qu'une seule analyse IA se fait à la fois pour ne pas exploser la VRAM
            await _lock.WaitAsync();
            try
            {
                File.AppendAllText(logPath, $"[{DateTime.Now:HH:mm:ss}] Verrou obtenu. Lancement de Task.Run...\n");
                return await Task.Run(() =>
                {
                    try
                    {
                        File.AppendAllText(logPath, $"[{DateTime.Now:HH:mm:ss}] Thread IA démarré.\n");
                    if (_processor != null)
                    {
                        File.AppendAllText(logPath, $"[{DateTime.Now:HH:mm:ss}] Processor OK. Chargement des images ({imagePaths.Length})...\n");
                        // Charge les images depuis le disque
                        using var images = Images.Load(imagePaths);
                        
                        File.AppendAllText(logPath, $"[{DateTime.Now:HH:mm:ss}] Traitement des images...\n");
                        // Prépare le prompt et les images pour le modèle
                        using var inputTensors = _processor.ProcessImages(prompt, images);

                        File.AppendAllText(logPath, $"[{DateTime.Now:HH:mm:ss}] Génération des Tensors terminée. Configuration du GeneratorParams...\n");
                        using var generatorParams = new GeneratorParams(_model);
                        // max_length correspond à la taille TOTALE (prompt + images + réponse).
                        // 1 image = ~3500 tokens. On met 5000 pour avoir un peu de marge.
                        generatorParams.SetSearchOption("max_length", 5000); 

                        File.AppendAllText(logPath, $"[{DateTime.Now:HH:mm:ss}] Création du Generator...\n");
                        using var generator = new Generator(_model, generatorParams);
                        generator.SetInputs(inputTensors);
                        List<int> generatedTokens = new List<int>();
                        
                        File.AppendAllText(logPath, $"[{DateTime.Now:HH:mm:ss}] Boucle de génération de tokens...\n");
                        // Exécute la génération complète
                        while (!generator.IsDone())
                        {
                            generator.GenerateNextToken();
                            var seq = generator.GetSequence(0);
                            generatedTokens.Add(seq[seq.Length - 1]);
                        }
                        
                        File.AppendAllText(logPath, $"[{DateTime.Now:HH:mm:ss}] Génération terminée. Décodage...\n");
                        // Décode uniquement la séquence générée (sans le prompt)
                        return _processor.Decode(generatedTokens.ToArray()).Trim();
                    }
                    else if (_tokenizer != null)
                    {
                        // Fallback text-only
                        using var tokens = _tokenizer.Encode(prompt);
                        using var generatorParams = new GeneratorParams(_model);
                        generatorParams.SetSearchOption("max_length", 150);
                        
                        using var generator = new Generator(_model, generatorParams);
                        generator.AppendTokenSequences(tokens);
                        
                        List<int> generatedTokens = new List<int>();
                        
                        while (!generator.IsDone())
                        {
                            generator.GenerateNextToken();
                            var seq = generator.GetSequence(0);
                            generatedTokens.Add(seq[seq.Length - 1]);
                        }
                        
                        return _tokenizer.Decode(generatedTokens.ToArray()).Trim();
                    }
                    
                    return "Erreur : le modèle n'a pas pu être chargé.";
                }
                catch (Exception ex)
                {
                    File.AppendAllText(System.IO.Path.Combine(System.IO.Path.GetTempPath(), "fern_ai_log.txt"), $"[{DateTime.Now:HH:mm:ss}] EXCEPTION IA RUN: {ex}\n");
                    return $"Erreur lors de l'analyse : {ex.Message}";
                }
            });
        }
        finally
        {
            _lock.Release();
            File.AppendAllText(System.IO.Path.Combine(System.IO.Path.GetTempPath(), "fern_ai_log.txt"), $"[{DateTime.Now:HH:mm:ss}] Verrou AnalyzeFramesAsync relâché.\n");
        }
        }

        public void Dispose()
        {
            _processor?.Dispose();
            _tokenizer?.Dispose();
            _model?.Dispose();
        }
    }
}
